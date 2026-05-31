# Progress Log — simpsonsarcade-ps3

## Phase overview

| Phase | Description | Status |
|---|---|---|
| 0. Recon | Find PKG, confirm same game as 360, cross-reference | ✅ Complete |
| 1. Extract | Unpack PSN PKG → EBOOT.BIN + game data | ✅ Complete |
| 2. Analysis | SELF/ELF structural analysis | ✅ Complete |
| 3. Decrypt | EBOOT.BIN (SELF) → EBOOT.elf | ✅ Complete |
| 4. Disasm/find | OPD + heuristic function discovery | ✅ Complete — 14,754 functions |
| 5. NID resolve | Import table → library/function names | 🔜 Next |
| 6. Lift | ppu_lifter → C++ | ⬜ |
| 7. Link | Build against ps3recomp runtime | ⬜ |
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

---

## Next steps
1. **Resolve import NIDs** (`prx_analyzer.py`, `nid_database.py`); diff against `flOw`'s 140-NID map.
   Expect a small subset (cellGcmSys, cellAudio, cellPad, cellFs, cellSysutil/Sysmodule,
   sysPrxForUser, sceNpTrophy/cellGame).
2. **First `ppu_lifter` pass** via `tools/recompile.py`; verify the 360's "21 missing instructions"
   are covered by ps3recomp's lifter.
3. **Apply the 360 speed-fix early** (timebase scaling for the PPE `mftb`).
4. **Use the 360 lifted source as an oracle** for the arcade-core functions.

## Open questions
1. How many functions does the PS3 EBOOT lift to vs. the 360's 15,237?
2. What's the `.SR` archive format? (the 360 build's loader is the oracle)
3. Does `SIMPSONS_FW.SR`'s size difference (16.5 vs 14.4 MB) reflect different shader/audio assets?
4. Does the PS3 build use any SPU programs, or is it PPE-only? (likely PPE-only — small arcade title)
