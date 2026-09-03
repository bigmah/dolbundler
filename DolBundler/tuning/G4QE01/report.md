# Mario Soccer (G4QE01) — tuning report

Written 2026-09-03T03:40:58Z by `tunegame` on this Mac, headless, phone-shaped (no fallback JIT, limiter off, Null video).
Recompiler 933fdd4; LLVM 20.1.8; codegen build `dolllvm-9b52bcfe59dcb161`; 64-instruction chunks.

## Speed on the bench scene (median over 40 s, x real time on this Mac)

| module | against tuned, by interleaved pairs | runs |
|---|---|---|
| **tuned** (scene-weighted profile) | **3.14x** median of 7 runs, 2.78-3.58x | 3.14/3.50/3.58/2.83/3.48/2.78/3.04 |
| generic (what `recompios send` collects alone) | +0.6% (pairs disagree, so no verdict; pairs 1.006, 0.996, 1.164) | 3.50/2.77/3.54 |
| chunk128 | -5.3% (all pairs agree; pairs 0.786, 0.947, 0.989) | 2.75/3.39/2.80 |

This machine's speed steps by up to a quarter between one run and the next (flat within a run), so only adjacent, interleaved pairs are compared and a difference counts only when every pair agrees on its sign.

Status: **needs-attention**. Boot run: 11.3 guest minutes at 2.10x median with the instrumented module.

## The bench scene

`~/Library/Application Support/DolBundler/tuning/G4QE01/scene.sav`: the state written 254 s into the boot run, where the speed around it was 2.01x, the slowest of 12 candidates past the logos; re-benched clean at 2.01x (instrumented). The others tried:

- t=169 s: 1.95x around it, 2.33x re-benched
- t=254 s: 2.01x around it, 2.01x re-benched
- t=296 s: 2.02x around it, 2.46x re-benched

## Health (tuned module, 40 s from the scene)

| counter | value | meaning |
|---|---|---|
| guest faults | 0 | invalid reads/writes; must be 0 or the scene is dying |
| fallback | 0 | guest instructions the interpreter ran (phone: same) |
| hook_fb | 0 | instructions hooks handed back |
| smc_lost | 0 B in 0 chunk(s) | guest code taken from the module after self-modification |
| poll_reads / yields | 67779311 / 0 | polls the chassis proved and skipped |
| vector_hle | 706352 | exception-vector stand-in hits |
| native dispatches | 711651182 | module entries over 63697219968 guest cycles |

## Where host time goes (200 µs samples on the CPU thread, tuned module)

| category | share |
|---|---|
| generated | 79.4% |
| chassis | 11.9% |
| other | 5.1% |
| video | 3.0% |
| exact-fp/psq | 0.5% |
| audio | 0.1% |

Hottest symbols:

- 64.5% `gG4QE01_func_802591C0_budget`
- 5.5% `_ZN16StaticRecompCore3RunEv`
- 2.4% `chassis_dispatch`
- 2.3% `_Z20TexDecoder_DecodeXFBPhPKhjjj`
- 1.0% `_ZN16StaticRecompCore20AdvanceGuestTimebaseEy`
- 0.9% `gG4QE01_func_80005100_budget`
- 0.9% `_ZN7PowerPC3MMU15WriteToHardwareILNS_13XCheckTLBFlagE2ELb0EEEvjjj`
- 0.6% `_ZN16StaticRecompCore17HookExternalWriteEP8CPUStatejyh`
- 0.6% `gG4QE01_func_80252AC0_budget`
- 0.6% `gG4QE01_func_801DFFC0_budget`
- 0.6% `_platform_memmove`
- 0.5% `gG4QE01_func_802529C0_budget`

## Hottest guest functions (merged profile, LLVM block counts)

| chunk | share of top 24 | function |
|---|---|---|
| `0x802591C0` | 98.7% | SelectThread (0x80259148, 552 bytes), chunk at +0x78 |
| `0x80005100` | 0.4% | __fill_mem (0x800050E4, 184 bytes), chunk at +0x1C |
| `0x801BA6C0` | 0.1% | unnamed 0x801BA038 (2848 bytes), chunk at +0x688 |
| `0x801D79C0` | 0.1% | glAttachPoly2(eGLView, unsigned long, glPoly2 *, unsigned long *, void *) (0x801D793C, 724 bytes), chunk at +0x84 |
| `0x801DFFC0` | 0.1% | 0x801DFFC0 (no function found) |
| `0x801DC0C0` | 0.1% | glGetTextureState(unsigned long long, eGLTextureState) (0x801DC054, 156 bytes), chunk at +0x6C |
| `0x8023AAC0` | 0.1% | __mod2i (0x8023AA4C, 268 bytes), chunk at +0x74 |
| `0x801BA5C0` | 0.0% | unnamed 0x801BA038 (2848 bytes), chunk at +0x588 |
| `0x801B58C0` | 0.0% | 0x801B58C0 (no function found) |
| `0x802100C0` | 0.0% | TLInstance::GetColour( (void)) (0x802100B0, 56 bytes), chunk at +0x10 |
| `0x801B5AC0` | 0.0% | GLMeshWriter::End(void) (0x801B5AB4, 144 bytes), chunk at +0xC |
| `0x801DC2C0` | 0.0% | glSetRasterState(eGLState, unsigned long) (0x801DC248, 180 bytes), chunk at +0x78 |
| `0x801DC1C0` | 0.0% | glSetRasterState(unsigned long &, eGLState, unsigned long) (0x801DC18C, 188 bytes), chunk at +0x34 |
| `0x801D8FC0` | 0.0% | cPlayer::PostPhysicsUpdate(void) (0x801D8FBC, 32 bytes), chunk at +0x4 |
| `0x802549C0` | 0.0% | DCInvalidateRange (0x802549C0, 44 bytes) |
| `0x801D8AC0` | 0.0% | cPlayer::PostPhysicsUpdate(void) (0x801D8AB4, 32 bytes), chunk at +0xC |

## Where the module returns to the chassis (every 4096th dispatch sampled)

- `0x80259294` ×74260 — SelectThread (0x80259148, 552 bytes), chunk at +0x14C
- `0x8023AB58` ×4407 — __shl2i (0x8023AB58, 36 bytes)
- `0x801DC148` ×3938 — glGetTextureState(eGLTextureState) (0x801DC0F0, 156 bytes), chunk at +0x58
- `0x801B5940` ×3060 — GLMeshWriter::Texcoord(nlVector2 &) (0x801B5940, 92 bytes)
- `0x801BA6D0` ×3027 — unnamed 0x801BA038 (2848 bytes), chunk at +0x698
- `0x801DFFF4` ×3010 — GLMeshWriterCore::Vertex(nlVector3 &) (0x801DFFF4, 76 bytes)
- `0x801E0090` ×2951 — GLMeshWriterCore::Colour(nlColour &) (0x801E0090, 36 bytes)
- `0x801BA67C` ×2945 — unnamed 0x801BA038 (2848 bytes), chunk at +0x644

## Leads

- **99% of the hottest-24's block iterations and 65% of host samples are in one wait loop at `0x80259294`** in SelectThread (0x80259148, 552 bytes), chunk at +0x78 -- 3 instructions, no store and no call in it (`tunegame disasm --disc-id G4QE01 --addr 0x80259294 --count 3` to read it off your own disc). It waits on plain RAM, so the chassis never sees the reads -- generated code does them inline and the poll-skip cannot fire. This is the shape Dolphin's own idle-skip (`IsBusyWaitLoop`) recognises: a self-loop of load, compare, branch with no store and no call, waiting for an interrupt handler to write the word. The fix is to recognise it in the recompiler's loop guard and hand the wait to the chassis so guest time advances to the next event, not to make the loop faster. `SelectThread`'s wait on the run-queue bits (r13-relative) is the SDK's idle; read tuning/README.md's dead ends first -- `PPCHalt` is a different loop and skipping it froze a title.

## Disassembly

Left out of the committed record on purpose: it would be the game's own instructions. `tunegame disasm --disc-id G4QE01` prints the same three chunks and the top dispatch site from your own copy of the disc, and the store's copy of this report has them inline.

## Variants tried

- **chunk128** — 128-instruction chunks, own profile. -5.3% by pairs (all agree); pairs 0.786, 0.947, 0.989; tuned arm 3.500/3.580/2.830; variant arm 2.750/3.390/2.800
- **generic** — the profile recompios collects on its own (boot run only). +0.6% by pairs (disagree); pairs 1.006, 0.996, 1.164; tuned arm 3.480/2.780/3.040; variant arm 3.500/2.770/3.540

## Files

- store: `~/Library/Application Support/DolBundler/tuning/G4QE01` (scene.sav, modules/, profiles/, runs/, results.json)
- tuned module: `~/Library/Application Support/DolBundler/tuning/G4QE01/modules/tuned-c64/module-build/gG4QE01_recomp.dylib`
- to re-measure a change: `tunegame ab --disc-id G4QE01 --a tuned --b <your dylib>` (`tuned` is rebuilt from the store's profile if `clean` removed it)
- to build a variant: `tunegame build --disc-id G4QE01 --out <dir> --pgo scene [--chunk N] [--env K=V]`
