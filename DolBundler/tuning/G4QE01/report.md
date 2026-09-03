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

`/Users/tonyradtke/Library/Application Support/DolBundler/tuning/G4QE01/scene.sav`: the state written 254 s into the boot run, where the speed around it was 2.01x, the slowest of 12 candidates past the logos; re-benched clean at 2.01x (instrumented). The others tried:

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

- **99% of the hottest-24's block iterations and 65% of host samples are in one wait loop at `0x80259294`** in SelectThread (0x80259148, 552 bytes), chunk at +0x78:
  ```
  80259294  lwz 0, -18584(13)
  80259298  cmplwi	0, 0
  8025929C  bt	2, .-8  -> 0x80259294
  ```
  It waits on plain RAM, so the chassis never sees the reads -- generated code does them inline and the poll-skip cannot fire. This is the shape Dolphin's own idle-skip (`IsBusyWaitLoop`) recognises: a self-loop of load, compare, branch with no store and no call, waiting for an interrupt handler to write the word. The fix is to recognise it in the recompiler's loop guard and hand the wait to the chassis so guest time advances to the next event, not to make the loop faster. `SelectThread`'s wait on the run-queue bits (r13-relative) is the SDK's idle; read tuning/README.md's dead ends first -- `PPCHalt` is a different loop and skipping it froze a title.

## Disassembly

A chunk is 64 instructions; `_budget` in a symbol is the same chunk's budget-carrying entry. The hottest chunk is shown whole, the next two by their first 24 instructions.

**SelectThread (0x80259148, 552 bytes), chunk at +0x78**

```
802591C0  7C840034  cntlzw	4, 4
802591C4  7C002000  cmpw	0, 4
802591C8  4181000C  bt	1, .+12  -> 0x802591D4
802591CC  38600000  li 3, 0
802591D0  48000188  b .+392  -> 0x80259358
802591D4  38000001  li 0, 1
802591D8  B00602C8  sth 0, 712(6)
802591DC  800602D0  lwz 0, 720(6)
802591E0  54001838  slwi 0, 0, 3
802591E4  7C1F0214  add 0, 31, 0
802591E8  900602DC  stw 0, 732(6)
802591EC  80A602DC  lwz 5, 732(6)
802591F0  80850004  lwz 4, 4(5)
802591F4  28040000  cmplwi	4, 0
802591F8  4082000C  bf	2, .+12  -> 0x80259204
802591FC  90C50000  stw 6, 0(5)
80259200  48000008  b .+8  -> 0x80259208
80259204  90C402E0  stw 6, 736(4)
80259208  908602E4  stw 4, 740(6)
8025920C  38000000  li 0, 0
80259210  38800001  li 4, 1
80259214  900602E0  stw 0, 736(6)
80259218  80A602DC  lwz 5, 732(6)
8025921C  90C50004  stw 6, 4(5)
80259220  800602D0  lwz 0, 720(6)
80259224  80ADB768  lwz 5, -18584(13)
80259228  2000001F  subfic 0, 0, 31
8025922C  7C800030  slw 0, 4, 0
80259230  7CA00378  or 0, 5, 0
80259234  900DB768  stw 0, -18584(13)
80259238  908DB76C  stw 4, -18580(13)
8025923C  A00601A2  lhz 0, 418(6)
80259240  540007BD  rlwinm. 0, 0, 0, 30, 30
80259244  40820018  bf	2, .+24  -> 0x8025925C
80259248  4BFFC099  bl .-16232  -> 0x802552E0 OSSaveContext
8025924C  28030000  cmplwi	3, 0
80259250  4182000C  bt	2, .+12  -> 0x8025925C
80259254  38600000  li 3, 0
80259258  48000100  b .+256  -> 0x80259358
8025925C  800DB768  lwz 0, -18584(13)
80259260  28000000  cmplwi	0, 0
80259264  40820054  bf	2, .+84  -> 0x802592B8
80259268  818DA568  lwz 12, -23192(13)
8025926C  3FC08000  lis 30, -32768
80259270  807E00E4  lwz 3, 228(30)
80259274  38800000  li 4, 0
80259278  7D8803A6  mtlr 12
8025927C  4E800021  blrl
80259280  38000000  li 0, 0
80259284  901E00E4  stw 0, 228(30)
80259288  387F0730  addi 3, 31, 1840
8025928C  4BFFBFED  bl .-16404  -> 0x80255278
80259290  4BFFD87D  bl .-10116  -> 0x80256B0C OSEnableInterrupts
80259294  800DB768  lwz 0, -18584(13)
80259298  28000000  cmplwi	0, 0
8025929C  4182FFF8  bt	2, .-8  -> 0x80259294
802592A0  4BFFD859  bl .-10152  -> 0x80256AF8 OSDisableInterrupts
802592A4  800DB768  lwz 0, -18584(13)
802592A8  28000000  cmplwi	0, 0
802592AC  4182FFE4  bt	2, .-28  -> 0x80259290
802592B0  387F0730  addi 3, 31, 1840
802592B4  4BFFC18D  bl .-15988  -> 0x80255440 OSClearContext
802592B8  38600000  li 3, 0
802592BC  906DB76C  stw 3, -18580(13)
```

**__fill_mem (0x800050E4, 184 bytes), chunk at +0x1C**

```
80005100  41820014  bt	2, .+20  -> 0x80005114
80005104  7CA32850  sub	5, 5, 3
80005108  3463FFFF  addic. 3, 3, -1
8000510C  9CE60001  stbu 7, 1(6)
80005110  4082FFF8  bf	2, .-8  -> 0x80005108
80005114  28070000  cmplwi	7, 0
80005118  4182001C  bt	2, .+28  -> 0x80005134
8000511C  54E3C00E  slwi 3, 7, 24
80005120  54E0801E  slwi 0, 7, 16
80005124  54E4402E  slwi 4, 7, 8
80005128  7C600378  or 0, 3, 0
8000512C  7C800378  or 0, 4, 0
80005130  7CE70378  or 7, 7, 0
80005134  54A3D97F  rlwinm. 3, 5, 27, 5, 31
80005138  3886FFFD  addi 4, 6, -3
8000513C  4182002C  bt	2, .+44  -> 0x80005168
80005140  90E40004  stw 7, 4(4)
80005144  3463FFFF  addic. 3, 3, -1
80005148  90E40008  stw 7, 8(4)
8000514C  90E4000C  stw 7, 12(4)
80005150  90E40010  stw 7, 16(4)
80005154  90E40014  stw 7, 20(4)
80005158  90E40018  stw 7, 24(4)
8000515C  90E4001C  stw 7, 28(4)
```

**unnamed 0x801BA038 (2848 bytes), chunk at +0x688**

```
801BA6C0  B0E48000  sth 7, -32768(4)
801BA6C4  B0E48000  sth 7, -32768(4)
801BA6C8  B0E48000  sth 7, -32768(4)
801BA6CC  4200FFDC  bdnz .-36  -> 0x801BA6A8
801BA6D0  7C083050  sub	0, 6, 8
801BA6D4  3C80CC01  lis 4, -13311
801BA6D8  7C0903A6  mtctr 0
801BA6DC  7C083040  cmplw	8, 6
801BA6E0  40800010  bf	0, .+16  -> 0x801BA6F0
801BA6E4  B0E48000  sth 7, -32768(4)
801BA6E8  39080001  addi 8, 8, 1
801BA6EC  4200FFF8  bdnz .-8  -> 0x801BA6E4
801BA6F0  38E70001  addi 7, 7, 1
801BA6F4  7C071840  cmplw	7, 3
801BA6F8  4180FF84  bt	0, .-124  -> 0x801BA67C
801BA6FC  48000234  b .+564  -> 0x801BA930
801BA700  808DAFE8  lwz 4, -20504(13)
801BA704  28040000  cmplwi	4, 0
801BA708  40820020  bf	2, .+32  -> 0x801BA728
801BA70C  480079F9  bl .+31224  -> 0x801C2104 dlGetSize(unsigned long)
801BA710  7C7D1B78  mr	29, 3
801BA714  807E0004  lwz 3, 4(30)
801BA718  48007A1D  bl .+31260  -> 0x801C2134 dlGetSize(unsigned long)
801BA71C  7FA4EB78  mr	4, 29
```

**Around the top dispatch site `0x80259294`** (where the module most often hands control back):

```
80259284  901E00E4  stw 0, 228(30)
80259288  387F0730  addi 3, 31, 1840
8025928C  4BFFBFED  bl .-16404  -> 0x80255278
80259290  4BFFD87D  bl .-10116  -> 0x80256B0C OSEnableInterrupts
80259294  800DB768  lwz 0, -18584(13)
80259298  28000000  cmplwi	0, 0
8025929C  4182FFF8  bt	2, .-8  -> 0x80259294
802592A0  4BFFD859  bl .-10152  -> 0x80256AF8 OSDisableInterrupts
802592A4  800DB768  lwz 0, -18584(13)
802592A8  28000000  cmplwi	0, 0
802592AC  4182FFE4  bt	2, .-28  -> 0x80259290
802592B0  387F0730  addi 3, 31, 1840
```

## Variants tried

- **chunk128** — 128-instruction chunks, own profile. -5.3% by pairs (all agree); pairs 0.786, 0.947, 0.989; tuned arm 3.500/3.580/2.830; variant arm 2.750/3.390/2.800
- **generic** — the profile recompios collects on its own (boot run only). +0.6% by pairs (disagree); pairs 1.006, 0.996, 1.164; tuned arm 3.480/2.780/3.040; variant arm 3.500/2.770/3.540

## Files

- store: `/Users/tonyradtke/Library/Application Support/DolBundler/tuning/G4QE01` (scene.sav, modules/, profiles/, runs/, results.json)
- tuned module: `/Users/tonyradtke/Library/Application Support/DolBundler/tuning/G4QE01/modules/tuned-c64/module-build/gG4QE01_recomp.dylib`
- to re-measure a change: `tunegame ab --disc-id G4QE01 --a tuned --b <your dylib>` (`tuned` is rebuilt from the store's profile if `clean` removed it)
- to build a variant: `tunegame build --disc-id G4QE01 --out <dir> --pgo scene [--chunk N] [--env K=V]`
