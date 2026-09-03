#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The runtime and the recompiler are forks pinned as submodules, one inside the
# next. This is the one place that knows the chain, and walks it:
#
#   ModernGekko                       bigmah/ModernGekko   dolbundler  <- ExpansionPak/ModernGekko  master
#     vendor/dolphin                  bigmah/RecompCore    dolbundler  <- ExpansionPak/RecompCore   moderngekko-vendor
#       DolRecomp                     bigmah/DolRecomp     dolbundler  <- ExpansionPak/DolRecomp    main
#   DolBundler/vendor/gc_controller   bigmah/nso_gc_macos  main
#
#   forks.sh status     branch, unpushed commits, dirty files, and whether the
#                       level above pins what is checked out, for every level
#   forks.sh push       push every fork, deepest first, and commit a re-pin at
#                       each level above it, the root included (root not pushed)
#   forks.sh upstream   fetch each fork's upstream and count the commits between
#   forks.sh checkout   put every level back on its branch, which a plain
#                       `git submodule update` leaves detached
#   forks.sh adopt      make the bigmah repos GitHub forks of their upstreams:
#                       renames the current repos aside, forks, pushes. The
#                       forks are public and this is one-way, so it asks first.
#
# The submodule URLs are https so anyone can clone. To push with a specific
# SSH key, rewrite them once, globally:
#   git config --global url."git@github.com:bigmah/".insteadOf "https://github.com/bigmah/"
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"

# path | branch | parent path | github repo | upstream github repo | upstream branch
# Deepest first: the order a push has to happen in.
LEVELS=(
  "ModernGekko/vendor/dolphin/DolRecomp|dolbundler|ModernGekko/vendor/dolphin|bigmah/DolRecomp|ExpansionPak/DolRecomp|main"
  "ModernGekko/vendor/dolphin|dolbundler|ModernGekko|bigmah/RecompCore|ExpansionPak/RecompCore|moderngekko-vendor"
  "ModernGekko|dolbundler|.|bigmah/ModernGekko|ExpansionPak/ModernGekko|master"
  "DolBundler/vendor/gc_controller|main|.|bigmah/nso_gc_macos||"
)

g() { git -C "$ROOT/$1" "${@:2}"; }
die() { echo "forks.sh: $*" >&2; exit 1; }
rel_in_parent() { [ "$2" = "." ] && echo "$1" || echo "${1#"$2"/}"; }
pinned() { g "$2" ls-files -s -- "$(rel_in_parent "$1" "$2")" | awk '{print $2}'; }
current_branch() { g "$1" symbolic-ref -q --short HEAD 2>/dev/null || echo "(detached)"; }
has_ref() { g "$1" rev-parse -q --verify "$2^{commit}" >/dev/null 2>&1; }

for_each_level() { # $1: function taking (path branch parent repo upstream upstream_branch)
  local l
  for l in "${LEVELS[@]}"; do IFS='|' read -r p br parent repo up ub <<<"$l"; "$1" "$p" "$br" "$parent" "$repo" "$up" "$ub"; done
}
for_each_level_topdown() {
  local i
  for ((i=${#LEVELS[@]}-1; i>=0; i--)); do IFS='|' read -r p br parent repo up ub <<<"${LEVELS[$i]}"; "$1" "$p" "$br" "$parent" "$repo" "$up" "$ub"; done
}

need_checkout() { [ -f "$ROOT/$1/.git" ] || [ -d "$ROOT/$1/.git" ] || die "$1 is not checked out; run: git -C $ROOT submodule update --init --recursive"; }

cmd_status() {
  printf '%-36s %-12s %-10s %-9s %-8s %s\n' path branch unpushed dirty pin ''
  one() {
    local p=$1 br=$2 parent=$3; need_checkout "$p"
    local head cur dirty unpushed pin pinstate note=''
    head=$(g "$p" rev-parse HEAD); cur=$(current_branch "$p")
    dirty=$(g "$p" status --porcelain | wc -l | tr -d ' ')
    if has_ref "$p" "origin/$br"; then unpushed=$(g "$p" rev-list --count "origin/$br..HEAD"); else unpushed='unfetched'; fi
    pin=$(pinned "$p" "$parent"); [ "$pin" = "$head" ] && pinstate=ok || pinstate="${pin:0:7}!=${head:0:7}"
    [ "$cur" = "$br" ] || note="not on $br; run forks.sh checkout"
    [ "$(g "$p" rev-parse --is-shallow-repository)" = true ] && note="$note (shallow)"
    printf '%-36s %-12s %-10s %-9s %-8s %s\n' "$p" "$cur" "$unpushed" "$dirty" "$pinstate" "$note"
  }
  for_each_level_topdown one
}

cmd_checkout() {
  one() {
    local p=$1 br=$2; need_checkout "$p"
    local cur; cur=$(current_branch "$p")
    if [ "$cur" = "$br" ]; then echo "$p: on $br"; return; fi
    if has_ref "$p" "$br" && ! g "$p" merge-base --is-ancestor "$br" HEAD; then
      echo "$p: HEAD is not ahead of the local $br; leaving it detached rather than lose commits" >&2; return
    fi
    g "$p" checkout -q -B "$br"
    has_ref "$p" "origin/$br" && g "$p" branch -q --set-upstream-to="origin/$br" "$br" || true
    echo "$p: now on $br at $(g "$p" rev-parse --short HEAD)"
  }
  for_each_level_topdown one
}

cmd_push() {
  one() {
    local p=$1 br=$2 parent=$3; need_checkout "$p"
    [ "$(current_branch "$p")" = "$br" ] || die "$p is not on $br; run forks.sh checkout first"
    [ -z "$(g "$p" status --porcelain --untracked-files=no)" ] || die "$p has uncommitted changes; commit them first"
    echo "== $p"
    g "$p" push -q -u origin "$br" && echo "   pushed $br to origin ($(g "$p" rev-parse --short HEAD))"
    local head pin rel; head=$(g "$p" rev-parse HEAD); pin=$(pinned "$p" "$parent"); rel=$(rel_in_parent "$p" "$parent")
    if [ "$pin" != "$head" ]; then
      g "$parent" add -- "$rel"
      g "$parent" commit -q -m "Re-pin $(basename "$p"): $(g "$p" log -1 --format=%s)" -- "$rel"
      echo "   re-pinned in ${parent}: $(g "$parent" log -1 --format='%h %s')"
    fi
  }
  for_each_level one
  echo; echo "The root repository has any re-pin commits above; push it when ready."
}

cmd_upstream() {
  one() {
    local p=$1 br=$2 up=$5 ub=$6; [ -n "$up" ] || return 0; need_checkout "$p"
    g "$p" remote get-url upstream >/dev/null 2>&1 || g "$p" remote add upstream "https://github.com/$up.git"
    if [ "$(g "$p" rev-parse --is-shallow-repository)" = true ]; then
      echo "$p is a shallow clone; fetching the history so the counts mean something" >&2
      g "$p" fetch -q --unshallow origin
    fi
    g "$p" fetch -q upstream
    printf '%-36s %4s ahead, %4s behind  %s %s\n' "$p" "$(g "$p" rev-list --count "upstream/$ub..HEAD")" "$(g "$p" rev-list --count "HEAD..upstream/$ub")" "$up" "$ub"
  }
  for_each_level_topdown one
}

cmd_adopt() {
  command -v gh >/dev/null || die "gh is required: https://cli.github.com"
  cat <<MSG
This turns bigmah/ModernGekko, bigmah/RecompCore and bigmah/DolRecomp into
GitHub forks of ExpansionPak's repositories, so that GitHub shows the lineage
and pull requests upstream are one click. For each one that is not a fork yet:

  1. rename the current repository to <name>-snapshot (its history, branches
     and any issues stay there; nothing is deleted)
  2. fork the upstream into bigmah/<name>
  3. push the local branch there, and make it the fork's default branch

A fork of a public repository is public. Everything on the dolbundler
branches becomes visible the moment it is pushed.
MSG
  if [ "${1:-}" != "--yes" ]; then read -r -p "Type 'fork' to go ahead: " ans; [ "$ans" = "fork" ] || die "nothing done"; fi
  one() {
    local p=$1 br=$2 repo=$4 up=$5; [ -n "$up" ] || return 0; need_checkout "$p"
    local owner=${repo%/*} name=${repo#*/} info
    info=$(gh repo view "$repo" --json isFork,parent -q '"\(.isFork) \(.parent.nameWithOwner // "")"' 2>/dev/null || echo missing)
    echo "== $repo"
    if [ "$info" = "true $up" ]; then
      echo "   already a fork of $up"
    else
      if [ "$info" != missing ]; then
        gh repo rename "$name-snapshot" --repo "$repo" --yes >/dev/null
        echo "   renamed the old $repo to $owner/$name-snapshot"
      fi
      gh repo fork "$up" --clone=false >/dev/null
      local i; for i in $(seq 1 30); do gh repo view "$repo" --json name >/dev/null 2>&1 && break; sleep 2; done
      echo "   forked $up into $repo"
    fi
    g "$p" push -q -u origin "$br"
    gh repo edit "$repo" --default-branch "$br" >/dev/null
    echo "   pushed $br and made it the default branch"
  }
  for_each_level one
}

case "${1:-}" in
  status)   cmd_status ;;
  push)     cmd_push ;;
  upstream) cmd_upstream ;;
  checkout) cmd_checkout ;;
  adopt)    cmd_adopt "${2:-}" ;;
  *) awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit 2 ;;
esac
