#!/usr/bin/env python3
"""Build the SDK intercept table for a game that has no decomp.

`tools/symbols.py` gets the address -> SDK-function-name table from a decomp
symbol map. That exists for Super Mario Strikers and for essentially nothing
else, and it is the single thing standing between this client and any other
disc. This derives the same table from the binary.

Two independent methods, because neither alone is trustworthy:

**Transfer** (`--reference-dol`/`--reference-inc`, the one that decides).
Fingerprint every function in a game whose table we already trust, fingerprint
every function in the target, and match. The fingerprint is the whole
instruction stream with only the fields a linker changes masked out -- the
displacement of a `bl`, the halves of an `addis`/`addi` address pair, an
r2/r13-relative offset -- so two builds of the same SDK function fingerprint
identically while two *different* functions almost never do. Registers are
kept, which is what makes it far sharper than a hash.

**Signature** (`--dsy`, the cross-check). Dolphin's `HashSignatureDB` checksum
against its shipped `totaldb.dsy`, ~10 000 signatures from many games. Its hash
drops register fields entirely, so it collides badly: on Strikers it "matches"
7 789 of 8 671 functions, 90% of a game that is not 90% library code, and 10%
of the intercepts it produces are wrong. Useful only as a second opinion on an
address the transfer already named.

Ambiguity is resolved by layout. A run of one-line accessors
(`AIGetStreamVolLeft`, `AIGetStreamVolRight`, ...) differs only in an operand
the mask removes, so they fingerprint alike -- but the linker emits them in
source order in both games, so an unambiguous match on either side brackets the
ambiguous ones and orders them.

    # what the method is worth, measured against the decomp's own answer
    python3 tools/sdk_signatures.py --dol generated/main.dol \
        --reference-dol generated/main.dol --reference-inc generated/sdk_symbols.inc \
        --verify generated/sdk_symbols.inc

    # a table for a disc with no decomp
    python3 tools/sdk_signatures.py --dol /tmp/gexe52/sys/main.dol \
        --reference-dol generated/main.dol --reference-inc generated/sdk_symbols.inc \
        --out /tmp/gexe52/sdk_symbols.inc
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DSY = os.path.join(HERE, "..", "Data", "Sys", "totaldb.dsy")
DEFAULT_AURORA = os.path.join(HERE, "..", "GXRuntime", "graphics", "aurora")

MAX_FUNCTION_BYTES = 0x8000
HEADER_FUNC_RE = re.compile(r"\b([A-Z][A-Za-z0-9_]+)\s*\(")


# --- the DOL ---------------------------------------------------------------

class Dol:
    """Text sections of a DOL, as one sparse address space."""

    def __init__(self, blob):
        self.sections = []
        offs = struct.unpack(">18I", blob[0x00:0x48])
        addrs = struct.unpack(">18I", blob[0x48:0x90])
        sizes = struct.unpack(">18I", blob[0x90:0xD8])
        self.entry = struct.unpack(">I", blob[0xE0:0xE4])[0]
        for i in range(7):                       # text sections only
            if offs[i] and sizes[i]:
                self.sections.append((addrs[i], blob[offs[i]:offs[i] + sizes[i]]))
        self.sections.sort()

    def word(self, addr):
        for base, data in self.sections:
            if base <= addr < base + len(data) - 3:
                return struct.unpack_from(">I", data, addr - base)[0]
        return None

    def contains(self, addr):
        return any(base <= addr < base + len(data) for base, data in self.sections)

    def iter_words(self):
        for base, data in self.sections:
            for off in range(0, len(data) & ~3, 4):
                yield base + off, struct.unpack_from(">I", data, off)[0]


def sign_ext(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def branch_target(addr, inst):
    """Target of a b/bl (opcode 18) or bc (opcode 16), or None."""
    op = inst >> 26
    if op == 18:
        li = sign_ext(inst & 0x03FFFFFC, 26)
        return (li if inst & 2 else addr + li) & 0xFFFFFFFF
    if op == 16:
        bd = sign_ext(inst & 0x0000FFFC, 16)
        return (bd if inst & 2 else addr + bd) & 0xFFFFFFFF
    return None


def analyze_function(dol, start):
    """(start, size) of the function at `start`, or None.

    Dolphin's rule: walk forward, remember the farthest forward branch target
    inside the function, and end at the first `blr`/`rfi` that no branch
    reaches past. Without the farthest-target part, any early return ends the
    function and the signature is a fragment.
    """
    farthest = start
    addr = start
    while addr - start < MAX_FUNCTION_BYTES:
        inst = dol.word(addr)
        if inst is None or inst == 0:
            return None
        if inst in (0x4E800020, 0x4C000064):        # blr, rfi
            if addr >= farthest:
                return (start, addr - start + 4)
        else:
            op = inst >> 26
            if op in (16, 18) and not (inst & 1):   # a branch that does not link
                target = branch_target(addr, inst)
                if target is not None and start < target < start + MAX_FUNCTION_BYTES:
                    farthest = max(farthest, target)
        addr += 4
    return None


def find_functions(dol):
    """Every function DolRecomp's guest will actually call into.

    Entry points come from `bl` targets -- an SDK function that nothing calls
    cannot be intercepted anyway -- plus the DOL's own entry, plus whatever
    starts immediately after a function that has already been identified,
    iterated to a fixed point so a run of adjacent library functions is picked
    up from the one call into the first of them.
    """
    candidates = {dol.entry}
    for addr, inst in dol.iter_words():
        if (inst >> 26) == 18 and (inst & 1) and not (inst & 2):   # bl, relative
            target = branch_target(addr, inst)
            if target is not None and dol.contains(target):
                candidates.add(target)

    functions = {}
    pending = sorted(candidates)
    while pending:
        nxt = []
        for start in pending:
            if start in functions or start & 3:
                continue
            found = analyze_function(dol, start)
            if not found:
                continue
            functions[start] = found[1]
            after = start + found[1]
            # Padding between functions is zero or `nop`; step over it.
            while dol.word(after) in (0, 0x60000000):
                after += 4
            if dol.contains(after) and after not in functions:
                nxt.append(after)
        pending = nxt
    return functions


# --- Dolphin's checksum ----------------------------------------------------

def code_checksum(dol, start, size):
    """HashSignatureDB::ComputeCodeChecksum, verbatim.

    Hashes the opcode plus the fields that do not move between builds, and
    deliberately drops the immediates -- which is the whole reason a signature
    survives being linked into a different game at a different address.
    """
    total = 0
    for addr in range(start, start + size, 4):
        opcode = dol.word(addr)
        if opcode is None:
            return None
        op = opcode & 0xFC000000
        op2 = 0
        op3 = 0
        auxop = op >> 26
        if auxop == 4:                                  # paired singles
            sub = opcode & 0x0000003F
            if sub in (0, 8, 16, 21, 22):
                op3 = opcode & 0x000007C0
        elif 7 <= auxop <= 15:                          # addi, muli, ...
            op2 = opcode & 0x03FF0000
        elif auxop in (19, 31, 63):
            op2 = opcode & 0x000007FF
        elif auxop == 59:
            op2 = opcode & 0x0000003F
            if op2 < 16:
                op3 = opcode & 0x000007C0
        elif 32 <= auxop < 56:
            op2 = opcode & 0x03FF0000
        total = ((total << 17) & 0xFFFE0000) | ((total >> 15) & 0x0001FFFF)
        total ^= (op | op2 | op3)
    return total & 0xFFFFFFFF


# --- the fingerprint -------------------------------------------------------
#
# One masked word per instruction. What gets masked is exactly what the linker
# is free to change between two games that share a library:
#
#   * `b`/`bl` displacement -- the callee sits somewhere else.
#   * `addis` immediate, and the immediate of whatever consumes that register
#     next -- the two halves of an absolute address.
#   * any r2/r13-relative displacement -- the small-data areas are laid out
#     per link.
#
# Everything else survives, registers included. That is the difference between
# this and Dolphin's checksum, which throws the register fields away and then
# cannot tell two functions apart at all.

D_FORM = set(range(32, 56)) | {14, 24, 25, 26, 27, 28, 29}   # loads/stores + arith imm


def fingerprint(dol, start, size):
    words = []
    tainted = set()          # registers currently holding the high half of an address
    for addr in range(start, start + size, 4):
        inst = dol.word(addr)
        if inst is None:
            return None
        op = inst >> 26
        rt = (inst >> 21) & 0x1F
        ra = (inst >> 16) & 0x1F
        if op == 18:                                  # b / bl: mask the displacement
            words.append(inst & 0xFC000003)
        elif op == 15:                                # addis: the high half
            words.append(inst & 0xFFFF0000)
            tainted = (tainted | {rt}) if ra == 0 or ra in tainted else (tainted - {rt})
        elif op in D_FORM:
            mask_imm = ra in (2, 13) or ra in tainted
            words.append(inst & 0xFFFF0000 if mask_imm else inst)
            # `addi`/`ori` completing an address pair keeps the taint on its
            # destination; anything else writing that register clears it.
            if op in (14, 24) and mask_imm:
                tainted = tainted | {rt}
            elif op in (14, 24, 25, 26, 27, 28, 29) or 32 <= op < 56 and op < 36:
                tainted = tainted - {rt}
        else:
            words.append(inst)
            if op == 31:                              # X-form writes RT (or RA)
                tainted = tainted - {rt}
    return tuple(words)


def fingerprint_all(dol, functions):
    """fingerprint -> [addresses], and address -> fingerprint."""
    by_fp = {}
    by_addr = {}
    for start, size in functions.items():
        fp = fingerprint(dol, start, size)
        if fp is None or len(fp) < 2:
            continue                                  # a one-instruction stub says nothing
        by_fp.setdefault(fp, []).append(start)
        by_addr[start] = fp
    for addrs in by_fp.values():
        addrs.sort()
    return by_fp, by_addr


def monotone_assign(pairs):
    """Pick the heaviest set of (ref, tgt) pairs that increases in both.

    Both binaries lay their libraries out in the same order, so a set of
    matches is only credible if it is monotone; and where a fingerprint occurs
    several times -- a run of one-line accessors that differ by an operand the
    mask removed -- monotonicity is the only thing that says which is which.
    Weighting by function length means a 300-instruction match is not thrown
    away to keep two 3-instruction coincidences.

    `pairs` is (ref, tgt, weight). Returns the chosen pairs, heaviest chain
    first-to-last, in O(n log n) with a Fenwick tree over target rank.
    """
    if not pairs:
        return []
    order = sorted(set(pairs), key=lambda p: (p[0], -p[1]))
    ranks = sorted({p[1] for p in order})
    rank = {t: i + 1 for i, t in enumerate(ranks)}
    size = len(ranks)
    tree_val = [0.0] * (size + 1)
    tree_idx = [-1] * (size + 1)
    best_val = [0.0] * len(order)
    parent = [-1] * len(order)

    def query(i):
        v, k = 0.0, -1
        while i > 0:
            if tree_val[i] > v:
                v, k = tree_val[i], tree_idx[i]
            i -= i & -i
        return v, k

    def update(i, v, k):
        while i <= size:
            if v > tree_val[i]:
                tree_val[i], tree_idx[i] = v, k
            i += i & -i

    for n, (_r, t, w) in enumerate(order):
        i = rank[t]
        base, k = query(i - 1)
        best_val[n] = base + w
        parent[n] = k
        update(i, best_val[n], n)

    n = max(range(len(order)), key=lambda i: best_val[i])
    chain = []
    while n != -1:
        chain.append(order[n])
        n = parent[n]
    return chain[::-1]


def transfer(ref_dol, ref_funcs, tgt_dol, tgt_funcs, long_enough=16,
             window=0x10000, drift=0x2000, min_support=2):
    """address-in-target -> address-in-reference, for every function matched.

    The obvious constraint -- that matches increase in both address spaces --
    is wrong here, and expensively so. Strikers links `os.a` after `gx.a`;
    Disney's Extreme Skate Adventure links it 200 KB earlier, before `dvd.a`.
    Whole libraries move relative to each other, so a single monotone chain
    through the matches keeps one library's worth and throws the rest away
    (81 of 191 known-good pairs, measured).

    What is true is that a library is contiguous and internally ordered, so
    inside one the difference `target - reference` is nearly constant. So a
    match is believed when *other* matches nearby agree with it about that
    difference. A three-instruction accessor that coincidentally fingerprints
    like something 300 KB away has no such company; a real one sits in a run of
    hundreds that all agree within a few KB.

    Long functions vote first, because their fingerprints do not collide;
    short ones are then admitted only where a long one nearby already fixed the
    offset.
    """
    ref_by_fp, ref_fp = fingerprint_all(ref_dol, ref_funcs)
    tgt_by_fp, tgt_fp = fingerprint_all(tgt_dol, tgt_funcs)

    def candidates(predicate):
        out = []
        for fp, refs in ref_by_fp.items():
            if not predicate(fp):
                continue
            for t in tgt_by_fp.get(fp, ()):
                for r in refs:
                    out.append((r, t, len(fp)))
        return out

    import bisect
    long_pairs = sorted(candidates(lambda fp: len(fp) >= long_enough))
    long_refs = [p[0] for p in long_pairs]

    def support(r, t, pairs, refs_index):
        """How many other matches near `r` agree that the offset is `t - r`."""
        lo = bisect.bisect_left(refs_index, r - window)
        hi = bisect.bisect_right(refs_index, r + window)
        delta = t - r
        n = 0
        for j in range(lo, hi):
            r2, t2, _w = pairs[j]
            if r2 == r:
                continue
            if abs((t2 - r2) - delta) <= drift:
                n += 1
        return n

    scored = [(support(r, t, long_pairs, long_refs), w, r, t)
              for r, t, w in long_pairs]
    anchors = one_to_one((r, t, sup, w) for sup, w, r, t in scored
                         if sup >= min_support)

    # The offsets the anchors established, as a sorted list to search.
    anchor_refs = sorted(anchors)
    anchor_list = [(r, anchors[r]) for r in anchor_refs]
    anchor_index = [r for r, _t in anchor_list]

    def offset_supported(r, t):
        lo = bisect.bisect_left(anchor_index, r - window)
        hi = bisect.bisect_right(anchor_index, r + window)
        delta = t - r
        return any(abs((t2 - r2) - delta) <= drift
                   for r2, t2 in anchor_list[lo:hi])

    short = [(r, t, w) for r, t, w in candidates(lambda fp: len(fp) < long_enough)
             if offset_supported(r, t)]
    infill = one_to_one((r, t, 1, w) for r, t, w in short)

    mapping = {}
    for r, t in anchors.items():
        mapping[t] = r
    for r, t in infill.items():
        if r not in anchors and t not in mapping:
            mapping[t] = r
    # The long-function anchors are returned separately because the alignment
    # brackets must only be built from those. One wrong short match -- a GX
    # function that fingerprints like a stub in ai.a 90 KB away -- lands between
    # two good anchors and makes the bracket span two libraries, and the
    # alignment then refuses the whole run rather than a single pair.
    return (mapping, sorted((r, t) for t, r in mapping.items()),
            sorted(anchors.items()))


def one_to_one(quads):
    """reference -> target, keeping the best-scoring pair for each side.

    A fingerprint can match several places; this resolves that by taking pairs
    in descending (support, length) order and refusing any whose reference or
    target has already been claimed.
    """
    best = {}
    used = set()
    for _sup, _w, r, t in sorted(((-sup, -w, r, t) for r, t, sup, w in quads)):
        if r in best or t in used:
            continue
        best[r] = t
        used.add(t)
    return best


def longest_increasing(pairs):
    """The longest run of (ref, tgt) anchors that increases in both."""
    return [(r, t) for r, t, _w in monotone_assign([(r, t, 1.0) for r, t in pairs])]


# --- alignment -------------------------------------------------------------
#
# Exact fingerprints only match a game built against the same SDK. Strikers is
# an April 2004 SDK and Disney's Extreme Skate Adventure a September 2002 one,
# and across that gap 2% of functions match exactly -- the libraries were
# recompiled, not relocated.
#
# What does survive is order. The linker emits an object file's functions in
# source order and the object files in link order, so `gx.a GXGeometry.o` sits
# in the same place relative to `gx.a GXFrameBuf.o` in both games even when
# every instruction inside has changed. So: anchor on what does match exactly,
# then align the runs between anchors as two ordered sequences, scoring pairs
# on how much of their instruction stream they still share. This is what a
# binary-diff tool does, and it is the only thing that reaches a function like
# GXBegin, which no signature database has and no exact match will find.

def ngrams(fp, n=3):
    ops = tuple(w >> 26 for w in fp)            # opcode only: registers moved too
    return {ops[i:i + n] for i in range(max(0, len(ops) - n + 1))}


def similarity(a, b, ga, gb):
    """0..1. Shared opcode trigrams, penalised for a size mismatch."""
    if not ga or not gb:
        return 0.0
    inter = len(ga & gb)
    if not inter:
        return 0.0
    jac = inter / len(ga | gb)
    ratio = min(len(a), len(b)) / max(len(a), len(b))
    return jac * (0.5 + 0.5 * ratio)


def align_segment(refs, tgts, ref_fp, tgt_fp, grams, threshold):
    """Needleman-Wunsch over two ordered runs of functions."""
    n, m = len(refs), len(tgts)
    if n == 0 or m == 0 or n * m > 4000000:
        return []
    GAP = -0.15
    sim = [[0.0] * m for _ in range(n)]
    for i, r in enumerate(refs):
        fa, ga = ref_fp[r], grams[0][r]
        row = sim[i]
        for j, t in enumerate(tgts):
            row[j] = similarity(fa, tgt_fp[t], ga, grams[1][t])
    score = [[0.0] * (m + 1) for _ in range(n + 1)]
    # An explicit traceback, not a re-comparison of accumulated sums: floating
    # point makes `score[i][j] == score[i-1][j-1] + sim[i-1][j-1]` fail for
    # diagonals it should accept, and the walk then slides down the gaps and
    # returns almost nothing.
    move = [[0] * (m + 1) for _ in range(n + 1)]      # 1 diagonal, 2 up, 3 left
    for i in range(1, n + 1):
        score[i][0] = score[i - 1][0] + GAP
        move[i][0] = 2
    for j in range(1, m + 1):
        score[0][j] = score[0][j - 1] + GAP
        move[0][j] = 3
    for i in range(1, n + 1):
        srow, prow, simrow, mrow = score[i], score[i - 1], sim[i - 1], move[i]
        for j in range(1, m + 1):
            diag = prow[j - 1] + simrow[j - 1]
            up = prow[j] + GAP
            left = srow[j - 1] + GAP
            if diag >= up and diag >= left:
                srow[j], mrow[j] = diag, 1
            elif up >= left:
                srow[j], mrow[j] = up, 2
            else:
                srow[j], mrow[j] = left, 3
    out = []
    i, j = n, m
    while i > 0 and j > 0:
        d = move[i][j]
        if d == 1:
            if sim[i - 1][j - 1] >= threshold:
                out.append((refs[i - 1], tgts[j - 1], sim[i - 1][j - 1]))
            i -= 1
            j -= 1
        elif d == 2:
            i -= 1
        else:
            j -= 1
    return out


def align(ref_dol, ref_funcs, tgt_dol, tgt_funcs, anchors, threshold, max_segment,
          drift=0x8000):
    """Name-carrying matches found by aligning the runs between anchors.

    Only between *consecutive anchors that agree about the offset*: two anchors
    either side of a library boundary bracket a run in the reference and an
    unrelated run in the target, and aligning those two produces confident
    nonsense. Requiring the bracket to sit inside one library is what keeps the
    result trustworthy.
    """
    ref_by_fp, ref_fp = fingerprint_all(ref_dol, ref_funcs)
    tgt_by_fp, tgt_fp = fingerprint_all(tgt_dol, tgt_funcs)
    grams = ({a: ngrams(f) for a, f in ref_fp.items()},
             {a: ngrams(f) for a, f in tgt_fp.items()})

    ref_sorted = sorted(ref_fp)
    tgt_sorted = sorted(tgt_fp)
    import bisect
    pairs = sorted(anchors)
    out = {}
    for k in range(len(pairs) - 1):
        (r_lo, t_lo), (r_hi, t_hi) = pairs[k], pairs[k + 1]
        if t_hi <= t_lo:
            continue
        # A library that grew between SDK revisions drifts the offset steadily
        # -- gx.a is 24 KB in the reference and 38 KB here -- so the tolerance
        # has to be a library's worth of growth, not a function's. What it still
        # rejects is a bracket whose ends are in different libraries, which move
        # by hundreds of kilobytes.
        if abs((t_hi - r_hi) - (t_lo - r_lo)) > drift:
            continue
        refs = ref_sorted[bisect.bisect_right(ref_sorted, r_lo):
                          bisect.bisect_left(ref_sorted, r_hi)]
        tgts = tgt_sorted[bisect.bisect_right(tgt_sorted, t_lo):
                          bisect.bisect_left(tgt_sorted, t_hi)]
        if len(refs) > max_segment or len(tgts) > max_segment:
            continue
        for r, t, sc in align_segment(refs, tgts, ref_fp, tgt_fp, grams, threshold):
            out[t] = (r, sc)
    return out


def load_dsy(path):
    """checksum -> (name, size, object) from Dolphin's signature database.

    Half the entries carry the library and object file the function came from,
    appended after a tab (`AIInit \tai.a AIDriver.o`). That tag is what makes
    the database usable despite its collisions: functions from one object file
    land contiguously, so a lone hit whose object file no neighbour shares is
    almost always the hash colliding with unrelated game code.
    """
    with open(path, "rb") as f:
        blob = f.read()
    count = struct.unpack_from("<I", blob, 0)[0]
    entries = {}
    off = 4
    for _ in range(count):
        if off + 136 > len(blob):
            break
        checksum, size = struct.unpack_from("<II", blob, off)
        raw = blob[off + 8:off + 136].split(b"\0", 1)[0].decode("latin-1")
        name, _, obj = raw.partition("\t")
        entries[checksum] = (name.strip(), size, obj.strip())
        off += 136
    return entries


def dsy_named(dol, functions, dsy_path):
    """address -> name for every function the signature database recognises."""
    db = load_dsy(dsy_path)
    out = {}
    for start, size in functions.items():
        hit = db.get(code_checksum(dol, start, size))
        if hit:
            out[start] = hit[0]
    return out


def object_coherent(objs, neighbours):
    """Which totaldb hits sit next to another hit from the same object file.

    The linker emits an object file's functions in one run, so a real match has
    company; a hash collision with unrelated game code stands alone.
    """
    addrs = sorted(objs)
    ok = {}
    for i, a in enumerate(addrs):
        obj = objs[a]
        if not obj:
            ok[a] = True           # half the database is untagged; absence of a
            continue               # tag is not evidence against the match
        lo = max(0, i - neighbours)
        hi = min(len(addrs), i + neighbours + 1)
        ok[a] = any(objs[addrs[j]] == obj for j in range(lo, hi) if j != i)
    return ok


def aurora_sdk_names(aurora_dir):
    """Names that appear as functions in Aurora's dolphin SDK headers."""
    names = set()
    hdr_dir = os.path.join(aurora_dir, "include", "dolphin")
    if not os.path.isdir(hdr_dir):
        sys.exit(f"error: Aurora dolphin headers not found at {hdr_dir}")
    for root, _dirs, files in os.walk(hdr_dir):
        for fn in files:
            if fn.endswith((".h", ".hpp")):
                with open(os.path.join(root, fn), errors="replace") as f:
                    for m in HEADER_FUNC_RE.finditer(f.read()):
                        names.add(m.group(1))
    return names


def parse_inc(path):
    """address -> name from a generated sdk_symbols.inc."""
    out = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*"([^"]+)"\s*\}', line)
            if m:
                out[int(m.group(1), 16)] = m.group(2)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dol", required=True, help="the game to build a table for")
    ap.add_argument("--reference-dol", help="a game whose table is already trusted")
    ap.add_argument("--reference-inc", help="that game's sdk_symbols.inc")
    ap.add_argument("--dsy", default=DEFAULT_DSY)
    ap.add_argument("--no-dsy", action="store_true", help="skip the cross-check")
    ap.add_argument("--no-align", action="store_true",
                    help="exact fingerprints only, no ordered alignment")
    ap.add_argument("--similarity", type=float, default=0.45,
                    help="minimum shared-trigram score to accept an aligned pair")
    ap.add_argument("--max-segment", type=int, default=2000,
                    help="skip a run between anchors longer than this")
    ap.add_argument("--neighbours", type=int, default=6,
                    help="how far to look for a function from the same object file")
    ap.add_argument("--aurora", default=DEFAULT_AURORA)
    ap.add_argument("--out")
    ap.add_argument("--verify", help="an existing .inc to score the result against")
    ap.add_argument("--override", action="append", default=[],
                    help="a file of `NAME 0xADDRESS` lines, verified by hand, that "
                         "wins over both methods (repeatable)")
    ap.add_argument("--names-out", help="write every transferred name, not just SDK ones")
    args = ap.parse_args()

    with open(args.dol, "rb") as f:
        dol = Dol(f.read())
    functions = find_functions(dol)
    print(f"target: {len(functions)} functions, entry 0x{dol.entry:08X}")

    sdk = aurora_sdk_names(args.aurora)
    named = {}          # address -> name, from the transfer
    all_named = {}      # address -> name, including non-SDK library functions

    dsy_here = dsy_named(dol, functions, args.dsy) if os.path.exists(args.dsy) else {}

    if args.reference_dol:
        with open(args.reference_dol, "rb") as f:
            ref = Dol(f.read())
        ref_funcs = find_functions(ref)
        ref_names = parse_inc(args.reference_inc) if args.reference_inc else {}
        print(f"reference: {len(ref_funcs)} functions, {len(ref_names)} known SDK symbols")
        mapping, anchors, strong = transfer(ref, ref_funcs, dol, functions)
        print(f"transfer: {len(strong)} strong anchors, {len(mapping)} functions matched "
              f"({100.0 * len(mapping) / max(len(ref_funcs), 1):.0f}% of the reference)")
        for tgt_addr, ref_addr in mapping.items():
            name = ref_names.get(ref_addr)
            if name:
                named[tgt_addr] = name
        print(f"transfer: {len(named)} of the reference's {len(ref_names)} SDK symbols "
              f"landed in the target")

        if not args.no_align:
            aligned = align(ref, ref_funcs, dol, functions, strong,
                            args.similarity, args.max_segment)
            held_out_ok = held_out_bad = 0
            added = 0
            for tgt_addr, (ref_addr, score) in aligned.items():
                name = ref_names.get(ref_addr)
                truth = dsy_here.get(tgt_addr)
                if name and truth:
                    if truth == name:
                        held_out_ok += 1
                    else:
                        held_out_bad += 1
                if name and tgt_addr not in named:
                    named[tgt_addr] = name
                    added += 1
            print(f"align: {len(aligned)} functions aligned, {added} new SDK names")
            total = held_out_ok + held_out_bad
            if total:
                print(f"align: on the {total} addresses the signature database also "
                      f"names, the alignment agrees {held_out_ok} times "
                      f"({100.0 * held_out_ok / total:.0f}%)")

        all_named = dict(named)

    dsy_names, dsy_objs = {}, {}
    if not args.no_dsy and os.path.exists(args.dsy):
        db = load_dsy(args.dsy)
        for start, size in functions.items():
            hit = db.get(code_checksum(dol, start, size))
            if hit:
                dsy_names[start] = hit[0]
                dsy_objs[start] = hit[2]
        shared = [a for a in named if a in dsy_names]
        agree = [a for a in shared if dsy_names[a] == named[a]]
        print(f"cross-check: totaldb names {len(dsy_names)} functions; of the "
              f"{len(shared)} it shares with the transfer it agrees on {len(agree)}")
        for a in sorted(set(shared) - set(agree))[:12]:
            print(f"    0x{a:08X}  transfer={named[a]}  totaldb={dsy_names[a]}")
        # Only where the transfer said nothing at all, and only for names Aurora
        # implements, is a totaldb guess worth taking -- and even then it is the
        # weaker source, so it is reported separately.
        added = 0
        coherent = object_coherent(dsy_objs, args.neighbours)
        # An SDK function exists once. A name the checksum "finds" at a dozen
        # addresses is the hash colliding with a dozen unrelated stubs, and
        # taking any one of them is a coin flip -- so take none.
        seen = {}
        for a, n in dsy_names.items():
            if n in sdk:
                seen[n] = seen.get(n, 0) + 1
        dropped_obj = dropped_dup = 0
        for a, n in sorted(dsy_names.items()):
            if a in named or n not in sdk:
                continue
            if seen[n] > 1:
                dropped_dup += 1
                continue
            if not coherent.get(a, False):
                dropped_obj += 1
                continue
            named[a] = n
            added += 1
        print(f"cross-check: dropped {dropped_obj} totaldb hits whose object file no "
              f"neighbour shares, and {dropped_dup} whose name it claims more than once")
        print(f"cross-check: totaldb contributed {added} intercepts the transfer missed")

    # Hand-verified addresses win. Some functions -- GXBegin, GXSetArray,
    # GXCopyDisp -- exist in every GameCube game but appear in no signature
    # database and changed enough between SDK revisions that no automatic
    # method reaches them. They were identified by reading the disassembly
    # against the reference, and that reading is more reliable than either
    # method here, so it is applied last and unconditionally.
    forced = {}
    for path in args.override:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                name, addr = line.split()
                forced[int(addr, 16)] = name
    for addr, name in forced.items():
        for a in [a for a, n in named.items() if n == name]:
            del named[a]
        named[addr] = name
    if forced:
        print(f"override: {len(forced)} hand-verified addresses applied")

    table = sorted((a, n) for a, n in named.items() if n in sdk)
    print(f"intercepts: {len(table)}")

    if args.verify:
        truth = parse_inc(args.verify)
        mine = dict(table)
        agree = {a for a in truth if mine.get(a) == truth[a]}
        wrong = {a: (truth[a], mine[a]) for a in truth if a in mine and mine[a] != truth[a]}
        missing = sorted(set(truth) - set(mine))
        extra = sorted(set(mine) - set(truth))
        print(f"\nverify against {args.verify}:")
        print(f"  agree      {len(agree)}/{len(truth)} "
              f"({100.0 * len(agree) / max(len(truth), 1):.1f}%)")
        print(f"  disagree   {len(wrong)}")
        print(f"  missing    {len(missing)}")
        print(f"  extra      {len(extra)}")
        for a, (t, m) in sorted(wrong.items())[:20]:
            print(f"    0x{a:08X}  truth={t}  got={m}")
        for a in missing[:20]:
            print(f"    missing 0x{a:08X} {truth[a]}")
        for a in extra[:20]:
            print(f"    extra   0x{a:08X} {mine[a]}")

    if args.names_out and all_named:
        with open(args.names_out, "w") as out:
            for a, n in sorted(all_named.items()):
                out.write(f"0x{a:08X} {n}\n")
        print(f"wrote {args.names_out}")

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
        with open(args.out, "w") as out:
            out.write("// Auto-generated by tools/sdk_signatures.py -- do not edit.\n")
            out.write("// address -> Aurora-implemented SDK function, identified by\n")
            out.write("// transferring a reference game's table through masked\n")
            out.write("// instruction-stream fingerprints, cross-checked against\n")
            out.write("// Dolphin's totaldb.dsy.\n")
            out.write(f"// {len(table)} intercepts out of {len(functions)} functions.\n\n")
            for addr, name in table:
                out.write(f'    {{ 0x{addr:08X}u, "{name}" }},\n')
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
