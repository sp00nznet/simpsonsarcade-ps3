# Progress Log — simpsonsarcade-ps3

## Phase overview

| Phase | Description | Status |
|---|---|---|
| 0. Recon | Find PKG, confirm same game as 360, cross-reference | ✅ |
| 1. Extract | Unpack PSN PKG → EBOOT.BIN + game data | ✅ |
| 2. Analysis | SELF/ELF structural analysis | ✅ |
| 3. Decrypt | EBOOT.BIN (SELF) → EBOOT.elf | ✅ |
| 4. Disasm/find | OPD + heuristic function discovery | ✅ — 3,813 functions |
| 5. NID resolve | Import table → library/function names | ✅ — 20 libs, 256 funcs |
| 6. Lift | ppu_lifter → C++ | ✅ — 5,019 functions, split chunks |
| 7. Shared harness | Rebuild on ps3recomp's boot harness | ✅ — bespoke runtime retired |
| 8. First boot | Enter recompiled CRT | ✅ |
| 9. SPU jobs | Lift + register the SPURS/CRI job binaries | ✅ — both dispatch |
| 10. Graphics | RSX → D3D12 via the live NV4097 engine | ✅ — RSX_LIVE_DRAW=1 |
| 11. Boot to menus | Intro videos → GAMER PROFILE → ROM select | ✅ |
| 12. Arcade core | Mount SIMPSONS.SR, run the emulator core | ✅ — Stage 1 plays |
| 13. Input | cellPad keyboard + XInput | ✅ |
| 14. Performance | Release build + inlined bounds check | ✅ — 16 → 28-49 fps |
| 15. 🍩 Playable | | ✅ |
| 16. Audio quality | Stutter | ⬜ |
| 17. UI artifacts | Font atlas drawn as a full-screen quad | ⬜ |
| 18. Frame rate | 28-49 fps → 60 | ⬜ |

## Detailed log

### 2026-05-31 — Project kickoff & recon

**Phase 0 — Recon (COMPLETE)**
- Located the PS3 PSN release in `D:\recomp\ps3games\simpsons` (RAR of two PKGs + instructions).
- Found the **already-playable Xbox 360 recomp** of the same game at
  `D:\recomp\360\ecco\simpsonsarcade` (XenonRecomp + ReXGlue) — the primary reference.
- Studied the `flOw` PS3 port (`D:\recomp\ps3games\flow`) as the canonical ps3recomp pipeline.
- Wrote [`docs/360-crossref.md`](docs/360-crossref.md): the 360↔PS3 overlap analysis.

**Phase 1 — Extract (COMPLETE)**
- Extracted the PSN PKG with `flow/extract_pkg.py` (retail AES-128-CTR).
- Content ID `UP0101-NPUB30563_00-SIMPSONSARCADEKO`, Title ID `NPUB30563`.
- Got `USRDIR/EBOOT.BIN` (1.63 MB SELF), `0B/SIMPSONS.SR`, `0B/SIMPSONS_FW.SR`, trophy pkg, art.
- **Key finding:** `0B/SIMPSONS.SR` is **2,748,416 bytes on both PS3 and 360** — the arcade
  ROM/data archive is shared across platforms. The EBOOT is an arcade-emulator wrapper.

**Phase 2 — Analysis (COMPLETE, pre-decrypt)**
- EBOOT.BIN is a retail NPDRM SELF, SCE header version 2, **key revision `0x16`**.
- Plaintext ELF header at file offset `0x90`: **PPC64, big-endian**, entry `0x00186900`.
- 8 program headers; text+rodata PT_LOAD @ `0x10000` (filesz `0x1628E8`), data/BSS @ `0x180000`
  (memsz `0xC1560`). Segment payloads are encrypted.
- Full notes in [`docs/binary-analysis.md`](docs/binary-analysis.md).

**Phase 3 — Decrypt (COMPLETE)**
- Decrypted `EBOOT.BIN` → `game/EBOOT.elf` (1,626,488 B) with `rpcs3 --decrypt`.
  No RAP needed — RPCS3's built-in key-rev-`0x16` keys handled the retail NPDRM SELF directly.
- RPCS3 emitted a fully reconstructed ELF (29 section headers) — parses cleanly with
  `ps3recomp/tools/elf_parser.py`. Entry `0x186900`, text PT_LOAD @ `0x10000` confirmed.

**Phase 4 — Function discovery (COMPLETE)**
- `ps3recomp/tools/find_functions.py` found **14,754 functions** (`analysis_functions.json`).
- vs. the 360 build's **15,237** functions — **96.8% overlap in count**. Strong confirmation it's
  the same codebase: same arcade core, slightly tighter PS3 toolchain output.

**Discovery — it's an arcade emulator (see [`docs/emulator-architecture.md`](docs/emulator-architecture.md))**
- EBOOT strings reference the ROM names directly (`Simpsons_4J.KON`, `..._TILES/SPRITES/SOUND/
  SAMPLES.ROM`), the `0B/%s.SR` load path, plus `YM2151`, `z80`, `DUPLICATE_SPRITE`/`REMOVE_SPRITE`.
- The EBOOT is a **Konami arcade emulator** (SEGA-Vintage/M2 wrapper) running the original 1991
  coin-op romset. We recompile the *emulator*; the game's 6809 code is interpreted from the ROM.

**Phase 5 — NID resolution (COMPLETE)**
- `prx_analyzer.py` returned 0 (it expects a PRX dynamic section; this is an ET_EXEC EBOOT).
- Parsed the `sys_proc_prx_param` lib.stub tables directly → **20 libraries, 256 imported functions**
  (`imports.json`). Verified NID algorithm against the binary (`_cellGcmInitBody` = `0x15BAE46B`).
- Core path: cellGcmSys (22), cellAudio (12), sys_io (5), sys_fs (15), cellSysutil (19),
  cellSysmodule (2), sysPrxForUser (20), cellGame (5), cellRtc (2), cellSpurs (9).
- Stub for first playable: sceNp/sceNp2/sceNpTrophy/sceNpCommerce2 (79), sys_net (14),
  cellNetCtl (6), cellSysutilAvc2 (9), cellUserInfo (1), cellSail (28), cellAtrac (8).

**Phase 6 — First lift (COMPLETE)**
- `ppu_lifter.py game/EBOOT.elf --functions analysis_functions.json` →
  **17,397 functions lifted** (14,754 + 2,643 mid-function tail-entry wrappers), 2,265 call targets.
- Output: `src/recomp/ppu_recomp.cpp` (119 MB), `ppu_recomp.h` (719 KB). (git-ignored)
- **Instruction coverage (first pass):** 4,784 TODOs. Real unhandled ops were few and known —
  `stfiwx` (532), op63 FP (247 incl. data), `cntlzd` (21), `mulhdu` (3) — same class the 360 patched.

**Phase 6b — Patch toolchain + re-lift (COMPLETE)**
- Patched `ps3recomp/tools/ppu_disasm.py` + `ppu_lifter.py`
  (`tools/patches/ps3recomp-missing-ppc-instructions.patch`):
  - disasm: `mulhdu` (xo=9), `stfiwx` (xo=983), `frsqrte`/`fre`/`frsqrtes`, and **fixed a real bug**
    where op63 conversions were mis-tabled (`846/847/814` → corrected to `814/815/846`), which had
    silently *swapped* `fctid`/`fcfid` and dropped `fctidz`.
  - lifter: `cntlzd`, `stfiwx`, `fctiw`, `frsqrte`/`frsqrtes`, `fre`/`fres`.
- Re-lift: TODOs **4,784 → 4,192**, and **0 real-instruction TODOs remain** — every one of the
  4,192 left is a `.word` data constant (opcode-0 padding / jump-table data misclassified as code,
  never executed). Instruction coverage for actual code is effectively complete.

**Toolchain patch upstreamed**
- Merged to `sp00nznet/ps3recomp` master via PR #2.

**Phase 8/9 — First build + boot (DONE 2026-05-31)**
- Reconciled the lifter's three symbol sets via `tools/gen_runtime.py` (header patch +
  `missing_stubs.cpp`) so all 18,628 `func_<addr>` resolve.
- MSVC fixes: added a `__builtin_clzll` polyfill to the lifter preamble (MSVC lacks it; pairs with
  the existing `__builtin_clz` one); defined the thread-local `g_trampoline_fn` (split-function
  fallthrough pointer) in `indirect_dispatch.cpp`.
- **The 119 MB `ppu_recomp.cpp` compiles** (single TU, `/bigobj`) and links against
  `ps3recomp_runtime.lib` → **`simpsons.exe` (17.9 MB)**.
- **First boot:** loads ELF segments, resolves entry OPD `0x186900` → code `0x00010230` /
  TOC `0x001913A8`, and executes the recompiled CRT. Stops at a null indirect call — next up is
  CRT bring-up (set `lr` to the `sys_process_exit` stub, prime TLS + heap, run constructors),
  mirroring the flОw sequence.

**Phase 7 — Runtime scaffold (COMPLETE)**
- Hand-written runtime (clean, no game-specific hacks — all parse under clang++ `-fsyntax-only`):
  - `src/config.h` — Simpsons constants (entry OPD 0x186900, text/data bases, heap region).
  - `src/elf_loader.{h,cpp}` — maps PT_LOAD segments; exposes the entry OPD.
  - `src/vm_bridge.cpp` — big-endian `vm_read*/vm_write*` over `vm_base` + LV2 syscall dispatch.
  - `src/malloc_override.cpp` — guest bump allocator (wire to the emulator's malloc later).
  - `src/indirect_dispatch.cpp` — `guest_addr -> host_func` map; `ps3_indirect_call`.
  - `src/hle_modules.cpp` — `g_ps3_module_registry`, `simpsons_hle()` NID dispatch (core libs
    routed, online/media stubbed), guest-callback trampoline.
  - `src/main.cpp` — boot sequence: vm_init → load ELF → deref entry OPD → enter recompiled CRT.
  - `CMakeLists.txt` — links the ps3recomp runtime (sibling `../../ps3`).
- `tools/gen_runtime.py` generates (into `src/recomp/`, git-ignored):
  - `func_table.cpp` — `g_recompiled_funcs[]` over all **18,114** lifted symbols.
  - `import_stubs.cpp` — the **256**-entry import table {stub_addr, nid, lib, name}.

---

## Next steps
1. **Audio stutter.** Plays, but breaks up. Not yet looked at.
2. **UI artifact.** Entering some menus draws the whole font/button atlas as one
   screen-filling quad. Intermittent. `LD_VMASK_DBG` showed the vertex-attribute
   analysis *correct* when it fired (`enabled=0x0109 analysed=0x0109`), so the
   mask-signature theory is not proven. `YZ_RSX_VERTEX_MODE=L` avoids it.
   Reproduce with `LD_PRESENT_DBG=1 LD_VMASK_DBG=1` to see whether a wrong
   surface reached the screen or a layout dropped an attribute.
3. **Frame rate** — 28-49 fps against 60. No longer single-thread bound: the load
   is ~1.8 cores over four threads with nothing pegged, so the next look is at
   pacing/synchronisation rather than raw throughput.
4. **`pso=434`** draw groups still rejected out of ~45k. Small, but not zero.

## Answered
1. *How many functions does the PS3 EBOOT lift to?* 3,813 detected, 5,019 lifted
   (the old 14,754/17,397 numbers were an over-aggressive detector; the current
   `find_functions.py` is tighter and the boot is healthier for it).
2. *Does the PS3 build use SPU programs?* **Yes** — and they are load-bearing.
   The whole render path goes through CRI middleware on a SPURS job chain, and
   nothing rendered until both job binaries were lifted and registered. They are
   raw images built in main memory, not ELFs in the EBOOT, so they only exist at
   the moment `cellSpurs` hands them over (`SPU_DUMP_MISS` captures them).

---

### 2026-09-01 — Rebuilt on the shared harness; playable

Retired the bespoke runtime (`elf_loader`/`vm_bridge`/`hle_modules`/`gcm_bridge`/
`indirect_dispatch`/`main.cpp`, all to `_attic/`) and rebuilt on ps3recomp's generic
boot harness, the same shape flOw and Twisted Metal use. That is what makes the
live NV4097 → D3D12 draw engine reachable: the harness selects it, so a port with
its own `main()` gets nothing.

Then five root causes, in order, each one gating the next:

1. **Two SPU job images were never registered.** The runtime already stages jm2
   jobs; with nothing registered every job logged `dispatch MISS`, so no command
   lists existed, so the fragment-program address pointed at 64 KB of zeros and
   all 92 draw groups were rejected in `get_pso`. Captured with `SPU_DUMP_MISS`,
   lifted, registered → the Konami logo appeared.
2. **`cellPadGetData` always reported a change packet.** This title *drains* the
   pad (`while (GetData(...) == OK && d.len > 0)`), so that loop never ended and
   the main thread pegged a core inside libpad. Fixed to one packet per host
   report. (Reporting on *change* was the first attempt and is wrong — a held
   button yields one packet.)
3. **No keyboard backend** — XInput only, and nothing plugged in.
4. **`cellSaveDataAutoSave2`** called a guest OPD as a host function pointer and
   crashed on first-boot profile creation.
5. **The harness never called `cellGame_init_from_paramsfo`**, so saves went to
   `BLES00000`.

Then two more, found by measurement rather than reading:

- **The flicker was one hardcoded argument.** `rsx_live_draw_present(0)` presents
  the surface registered for display buffer 0; this title flips `0,1,0,1…`, so
  half its frames showed the previous image. Ruled out lost FIFO commands (zero
  resyncs) and torn texture reads (`LD_TEX_RACE`: 0 torn out of 14,336) first.
  The "black flashes" turned out to be `PrintWindow` failing on a flip-model swap
  chain — a measurement artifact, not the game.
- **The build was Debug.** CMake fills `CMAKE_BUILD_TYPE` in during `project()`
  for MSVC-like toolchains and picks Debug; on a 25 MB generated translation unit
  that is the whole game unoptimised. 16 → 49 fps. Found by sampling the hot
  thread's RIP externally, after the built-in guest profiler proved useless (it
  samples `ctx.cia`, stale outside syscalls).
