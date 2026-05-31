# Progress Log — simpsonsarcade-ps3

## Phase overview

| Phase | Description | Status |
|---|---|---|
| 0. Recon | Find PKG, confirm same game as 360, cross-reference | ✅ Complete |
| 1. Extract | Unpack PSN PKG → EBOOT.BIN + game data | ✅ Complete |
| 2. Analysis | SELF/ELF structural analysis | ✅ Complete (pre-decrypt) |
| 3. Decrypt | EBOOT.BIN (SELF) → EBOOT.elf | 🔜 Next |
| 4. Disasm/find | OPD + heuristic function discovery | ⬜ |
| 5. NID resolve | Import table → library/function names | ⬜ |
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

---

## Next steps
1. **Decrypt EBOOT.BIN** → `game/EBOOT.elf` (RPCS3 "Decrypt PS3 binaries", or `ps3sce`/scetool with
   retail keys). The `GAMEDATA1.EDAT` will additionally need the RAP/klicensee if used.
2. Run `../../ps3/tools/elf_parser.py` + `find_functions.py`; compare function count to 360's 15,237.
3. Resolve import NIDs (`prx_analyzer.py`, `nid_database.py`); diff against `flOw`'s 140-NID map.
4. First `ppu_lifter` pass; verify the 360's "21 missing instructions" are covered.

## Open questions
1. How many functions does the PS3 EBOOT lift to vs. the 360's 15,237?
2. What's the `.SR` archive format? (the 360 build's loader is the oracle)
3. Does `SIMPSONS_FW.SR`'s size difference (16.5 vs 14.4 MB) reflect different shader/audio assets?
4. Does the PS3 build use any SPU programs, or is it PPE-only? (likely PPE-only — small arcade title)
