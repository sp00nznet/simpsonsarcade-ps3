# PS3 Binary Analysis — The Simpsons Arcade Game (NPUB30563)

## PKG container

Distributed as a retail PSN PKG (RAR-packed for distribution):

- `The Simpsons Arcade Game - C00 by Videogames SCZ.pkg` — 21,893,200 B, 21 items
- `C00 UNLOCK.pkg` — 283,808 B (DLC/unlock content)
- Magic `\x7FPKG`, content ID `UP0101-NPUB30563_00-SIMPSONSARCADEKO`
- Extracted with `flow/extract_pkg.py` (retail AES key, AES-128-CTR)

### Extracted layout (`extracted/`)
```
USRDIR/EBOOT.BIN            1,629,232 B   SELF (encrypted PPC64 executable)
USRDIR/0B/SIMPSONS.SR       2,748,416 B   arcade ROM + core data (== 360 build)
USRDIR/0B/SIMPSONS_FW.SR   16,552,976 B   asset/firmware archive
USRDIR/GAMEDATA1.EDAT             320 B   NPDRM-protected data (needs RAP)
TROPDIR/NPWR01444_00/TROPHY.TRP 534,272 B trophy package
PARAM.SFO, ICON0.PNG, PIC0–2.PNG, PS3LOGO.DAT, C00/...   metadata & art
```

## EBOOT.BIN — SELF header

| Field | Value |
|---|---|
| Magic | `SCE\0` |
| Header version | `0x00000002` |
| Key revision | `0x0016` (retail) |
| SCE type | `0x0001` (SELF) |
| Metadata offset | `0x000004A0` |
| NPDRM | yes |

The ELF header is plaintext at file offset `0x90`; **segment payloads are encrypted** (sampled
PT_LOAD #2 data is high-entropy). Decryption required before lifting.

## ELF (visible header) — PPC64 big-endian

| Field | Value |
|---|---|
| Class | ELF64 |
| Endian | big-endian (2) |
| Machine | `0x15` (PPC64) |
| Entry | `0x00186900` |
| Program headers | 8 (phentsize 56) |

### Program headers
| # | Type | Flags | Offset | VAddr | FileSz | MemSz | Notes |
|---|---|---|---|---|---|---|---|
| 0 | PT_LOAD | `0x400005` (R-X) | `0x0` | `0x10000` | `0x1628E8` | `0x1628E8` | **.text + rodata** |
| 1 | PT_LOAD | `0x600006` (RW-) | `0x170000` | `0x180000` | `0x1A804` | `0xC1560` | **.data + .bss** |
| 5 | PT_TLS | `0x4` | `0x17E35C` | `0x18E35C` | `0x4` | `0x110` | TLS template |
| 6 | `0x60000001` | — | `0x162880` | `0x172880` | `0x28` | `0x28` | PS3 PROC_PARAM |
| 7 | `0x60000002` | — | `0x1628A8` | `0x1728A8` | `0x40` | `0x40` | PS3 PRX/MODULE info |

(Types 2–4 are zero-size NPDRM/SCE padding segments.)

## Memory map (post-decrypt expectation)

- **Text/rodata**: `0x00010000` … `0x001728E8` (~1.45 MB)
- **Data/BSS**: `0x00180000` … `0x00241560` (filesz `0x1A804`, rest BSS)
- Same `0x10000` text base as the `flOw` port — standard PS3 user ELF layout.

## Next steps
1. Decrypt SELF → `game/EBOOT.elf` (RPCS3 / `ps3sce`).
2. `python ../../ps3/tools/elf_parser.py` for sections/segments/OPD/TOC.
3. `find_functions.py` (OPD + heuristic) → function table; compare with 360's 15,237.
4. `prx_analyzer.py` + `nid_database.py` → resolve import NIDs.

See [`360-crossref.md`](360-crossref.md) for how the 360 build informs every step.
