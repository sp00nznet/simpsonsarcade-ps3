# Progress Log — simpsonsarcade-ps3

## Phase overview

| Phase | Description | Status |
|---|---|---|
| 0. Recon | Find PKG, confirm same game as 360, cross-reference | ✅ Complete |
| 1. Extract | Unpack PSN PKG → EBOOT.BIN + game data | ✅ Complete |
| 2. Analysis | SELF/ELF structural analysis | ✅ Complete |
| 3. Decrypt | EBOOT.BIN (SELF) → EBOOT.elf | ✅ Complete |
| 4. Disasm/find | OPD + heuristic function discovery | ✅ Complete — 14,754 functions |
| 5. NID resolve | Import table → library/function names | ✅ Complete — 20 libs, 256 funcs |
| 6. Lift | ppu_lifter → C++ | ✅ Complete — 17,397 funcs, 119 MB |
| 7. Link | Build against ps3recomp runtime | 🔜 Next |
| 8. Boot | Reach main() / CRT init | ⬜ |
| 9. Arcade core | Mount SIMPSONS.SR, run emulator core | ⬜ |
| 10. GPU/Audio/Input | RSX→D3D12, cellAudio, cellPad | ⬜ |
| 11. Playable | 🍩 | ⬜ |

---

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

---

## Next steps
1. **Scaffold the runtime** (`src/main.cpp`, `elf_loader`, `hle_modules`, `import_stubs`,
   `func_table`, `malloc_override`) from the `flОw` port; wire the 10 core HLE modules.
2. **Link** against ps3recomp and chase first boot → CRT init → emulator ROM load (`0B/SIMPSONS.SR`).
3. **Apply the 360 speed-fix early** (timebase scaling for the PPE `mftb`).
4. **Upstream** the toolchain patch to the ps3recomp repo (benefits flОw / Tokyo Jungle too).

## Open questions
1. How many functions does the PS3 EBOOT lift to vs. the 360's 15,237?
2. What's the `.SR` archive format? (the 360 build's loader is the oracle)
3. Does `SIMPSONS_FW.SR`'s size difference (16.5 vs 14.4 MB) reflect different shader/audio assets?
4. Does the PS3 build use any SPU programs, or is it PPE-only? (likely PPE-only — small arcade title)
