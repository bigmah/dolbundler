#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the ModernGekko runtime and assemble DolBundler.app.
#
#   ./build.sh              build what is missing, then install to ~/Applications
#   ./build.sh --rebuild    force a full ModernGekko rebuild first
#   ./build.sh --no-install leave DolBundler.app here instead of installing it
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MG_SRC="$ROOT/ModernGekko"
MG_BUILD="$MG_SRC/build"
APP="$HERE/DolBundler.app"
INSTALL_DIR="$HOME/Applications"

REBUILD=0
INSTALL=1
for arg in "$@"; do
  case "$arg" in
    --rebuild) REBUILD=1 ;;
    --no-install) INSTALL=0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

bold=$'\033[1m'; off=$'\033[0m'
step() { printf '\n%s==> %s%s\n' "$bold" "$*" "$off"; }

for tool in cmake ninja git python3 cargo; do
  command -v "$tool" >/dev/null || { echo "$tool is required but not installed" >&2; exit 1; }
done

# Where cmake and ninja live, deduplicated and absolute. toolchain.conf carries
# it to the app, whose PATH when launched from Finder is launchd's four system
# directories - no Homebrew - which is where recompgc's "cmake is required" came
# from even on a machine that has it.
TOOLCHAIN_PATH=''
for tool in cmake ninja; do
  d="$(cd "$(dirname "$(command -v "$tool")")" && pwd)"
  case ":$TOOLCHAIN_PATH:" in
    *":$d:"*) ;;
    *) TOOLCHAIN_PATH="${TOOLCHAIN_PATH:+$TOOLCHAIN_PATH:}$d" ;;
  esac
done

# ModernGekko, RecompCore (vendor/dolphin) and DolRecomp all live directly in
# this repo, so a plain `git clone` already has them. Check anyway: a truncated
# or half-copied tree fails later, in CMake, with a much less obvious message.
step "Checking the runtime sources"
for f in "$MG_SRC/CMakeLists.txt" \
         "$MG_SRC/vendor/dolphin/CMakeLists.txt" \
         "$MG_SRC/vendor/dolphin/DolRecomp/CMakeLists.txt"; do
  [ -f "$f" ] || {
    echo "missing $f" >&2
    echo "That is part of this repo, so the checkout at $ROOT is incomplete." >&2
    exit 1
  }
done
echo "    ModernGekko, RecompCore and DolRecomp are in-tree"

# Dolphin's third-party externals are the one thing still fetched separately.
# They are upstream submodules pinned here, so a plain clone leaves them empty.
step "Checking out Dolphin's externals"
if [ ! -f "$MG_SRC/vendor/dolphin/Externals/fmt/fmt/CMakeLists.txt" ]; then
  echo "    fetching the externals (this is a few hundred MB)"
  git -C "$ROOT" submodule update --init --recursive --depth 1
else
  echo "    already present"
fi

# DolBundler needs two fixes in ModernGekko that upstream has not made. Both
# live in patches/ as ordinary diffs; see DolBundler/README.md for what each one
# is for. Applying is idempotent: a patch that reverse-applies cleanly is
# already in the tree.
step "Patching ModernGekko"
for patch in "$HERE"/patches/*.patch; do
  name="$(basename "$patch")"
  if git -C "$MG_SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "    $name (already applied)"
  elif git -C "$MG_SRC" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$MG_SRC" apply "$patch"
    echo "    $name (applied)"
  else
    echo "$name does not apply to $MG_SRC and is not already in the tree." >&2
    echo "Check out the pinned ModernGekko revision, or apply it by hand." >&2
    exit 1
  fi
done

step "Configuring ModernGekko"
if [ "$REBUILD" -eq 1 ]; then rm -rf "$MG_BUILD"; fi
# Keyed on build.ninja, not CMakeCache.txt: the cache is written before
# generation, so an interrupted or failed configure leaves one behind and
# "reusing" it would just hand ninja a directory with nothing to build.
if [ ! -f "$MG_BUILD/build.ninja" ]; then
  # CMake 4 refuses the pre-3.5 minimums a few of Dolphin's externals declare.
  cmake -S "$MG_SRC" -B "$MG_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
else
  echo "    reusing $MG_BUILD"
fi

step "Building the runtime, recompiler, and disc tools"
echo "    a cold build compiles all of Dolphin and takes a while"
cmake --build "$MG_BUILD" -j "$(sysctl -n hw.ncpu)" \
  --target moderngekko-run moderngekko-launcher moderngekko-port

for tool in ModernGekko moderngekko-run moderngekko-port dolrecomp; do
  [ -x "$MG_BUILD/$tool" ] || { echo "expected $MG_BUILD/$tool to exist" >&2; exit 1; }
done

# gc_controller is the driver for the Nintendo Switch Online GameCube
# controller, which SDL enumerates but cannot drive - it feeds Dolphin's Pipe
# input backend instead. It is vendored at DolBundler/vendor/gc_controller
# (see the PROVENANCE.md there); it used to be a checkout you had to have
# sitting beside this repo, which meant a clone that built cleanly still could
# not drive the one pad this project most wants to support. GC_CONTROLLER_DIR
# still points the build at a checkout of your own instead. Nothing here
# depends on the driver, so an unbuildable tree is a note, not a failure.
step "Building the GameCube controller driver"
GC_CONTROLLER=""
for candidate in "${GC_CONTROLLER_DIR:-}" "$HERE/vendor/gc_controller"; do
  [ -n "$candidate" ] || continue
  if grep -q '^name = "gc_controller"' "$candidate/Cargo.toml" 2>/dev/null; then
    GC_CONTROLLER="$(cd "$candidate" && pwd)"
    break
  fi
done
if [ -z "$GC_CONTROLLER" ]; then
  echo "    no driver source at $HERE/vendor/gc_controller; the pipe controller option will not work" >&2
elif cargo build --release --manifest-path "$GC_CONTROLLER/Cargo.toml" 2>&1 | tail -3; then
  echo "    $GC_CONTROLLER"
else
  echo "    $GC_CONTROLLER did not build; the pipe controller option will not work" >&2
fi

step "Building the DolBundler window"
cargo build --release --manifest-path "$HERE/gui/Cargo.toml"
GUI_BIN="$HERE/gui/target/release/DolBundler"
[ -x "$GUI_BIN" ] || { echo "expected $GUI_BIN to exist" >&2; exit 1; }

step "Assembling DolBundler.app"
rm -rf "$APP"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"
mkdir -p "$MACOS" "$RES"

install -m 755 "$GUI_BIN" "$MACOS/DolBundler"
install -m 755 "$HERE/src/recompgc" "$RES/recompgc"
install -m 755 "$HERE/src/recompios" "$RES/recompios"
install -m 644 "$HERE/src/make_game_app.py" "$RES/make_game_app.py"
python3 "$HERE/src/make_app_icon.py" --out "$RES/icon.icns" >/dev/null

cat > "$RES/toolchain.conf" <<CONF
# Written by DolBundler/build.sh. Absolute paths to the ModernGekko build that
# extracts, recompiles, and runs discs. Re-run build.sh if the checkout moves.
# REPO_ROOT is the checkout itself: recompios builds the iPhone app out of it,
# which needs ios/ and the sources beneath it, not only the built tools.
REPO_ROOT=$(printf '%q' "$ROOT")
MG_SRC=$(printf '%q' "$MG_SRC")
MG_BUILD=$(printf '%q' "$MG_BUILD")
APPS_DIR=$(printf '%q' "$INSTALL_DIR")
GRAPHICS_BACKEND=Metal
GC_CONTROLLER=$(printf '%q' "$GC_CONTROLLER")
# Where cmake and ninja were found. A Finder-launched app gets launchd's
# PATH, which has no Homebrew on it, so recompgc puts these back itself.
TOOLCHAIN_PATH=$(printf '%q' "$TOOLCHAIN_PATH")
CONF

# Written whole rather than patched: PlistBuddy has no upsert and the key set
# here is fixed.
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>DolBundler</string>
  <key>CFBundleDisplayName</key><string>DolBundler</string>
  <key>CFBundleExecutable</key><string>DolBundler</string>
  <key>CFBundleIdentifier</key><string>gc.dolbundler.app</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>GameCube or Wii disc image</string>
      <key>CFBundleTypeRole</key><string>Viewer</string>
      <key>LSHandlerRank</key><string>Alternate</string>
      <key>CFBundleTypeExtensions</key>
      <array>
        <string>iso</string>
        <string>gcm</string>
        <string>wbfs</string>
        <string>rvz</string>
        <string>ciso</string>
        <string>gcz</string>
      </array>
    </dict>
  </array>
</dict>
</plist>
PLIST

codesign --force --sign - "$APP" >/dev/null 2>&1 || true
touch "$APP"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" || true

if [ "$INSTALL" -eq 1 ]; then
  step "Installing to $INSTALL_DIR"
  mkdir -p "$INSTALL_DIR"
  rm -rf "$INSTALL_DIR/DolBundler.app"
  cp -R "$APP" "$INSTALL_DIR/DolBundler.app"
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$INSTALL_DIR/DolBundler.app" || true
  echo "    $INSTALL_DIR/DolBundler.app"
fi

printf '\n%sDone.%s Open DolBundler and add a disc image, or from a shell:\n' "$bold" "$off"
printf '  open -a DolBundler "<disc.iso>"      # window, live log, library\n'
printf '  %s "<disc.iso>"   # same pipeline, no window\n' "$RES/recompgc"
printf '  %s send                # every library game onto a connected iPhone\n' "$RES/recompios"
