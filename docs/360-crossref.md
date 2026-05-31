# 360 ↔ PS3 Cross-Reference

How much of the **already-playable Xbox 360 recomp** of The Simpsons Arcade Game lines up with the
PS3 PSN release — and how we exploit it to drive ps3recomp.

Source of truth for the 360 side: `D:\recomp\360\ecco\simpsonsarcade` (XenonRecomp + ReXGlue,
booting through menus + gameplay + audio + input at full speed).

---

## 1. It's the same game

| Identity | Xbox 360 | PS3 |
|---|---|---|
| Title | The Simpsons Arcade | The Simpsons Arcade Game |
| Title ID | `0x584111FA` | `NPUB30563` |
| Content ID | — (STFS LIVE) | `UP0101-NPUB30563_00-SIMPSONSARCADEKO` |
| Dev / Pub | Backbone Entertainment / Konami | Backbone Entertainment / Konami |
| Release | Feb 2012 (XBLA) | 2012 (PSN) |

## 2. The game data is shared across platforms — *this is the big one*

Both platforms ship the identical resource layout under `0B/`:

| File | 360 size | PS3 size | Match |
|---|---|---|---|
| `0B/SIMPSONS.SR` | 2,748,416 B | 2,748,416 B | ✅ **byte-for-byte identical size** |
| `0B/SIMPSONS_FW.SR` | 14,419,520 B | 16,552,976 B | same format, platform-specific assets |

`SIMPSONS.SR` is the **arcade ROM + core game data archive**. Being identical in size strongly
implies the original 1991 Konami coin-op data is shipped unchanged on both platforms — the EBOOT,
like the XEX, is an **emulator wrapper** around that ROM. `SIMPSONS_FW.SR` ("firmware"/asset pack)
differs because front-end assets (shaders, audio encodings, UI) are platform-specific.

**Implication:** whatever the 360 build does to mount, parse, and feed `SIMPSONS.SR` into the
arcade core is the same work the PS3 build does. The 360 recomp's `.SR` handling is a roadmap.

## 3. Same ISA family

Both are **64-bit big-endian PowerPC with VMX/AltiVec** vector units:

- Xenon (360): 3-core PPC, VMX128.
- Cell PPE (PS3): 1 PPC core (+ 6 SPUs), standard VMX/AltiVec.

The recompiled C++ from XenonRecomp and from ps3recomp's `ppu_lifter` operate on the same kind of
register file (GPR/FPR/VR/CR/LR/CTR/XER/FPSCR) with the same byte-swap discipline. Instruction
coverage overlaps heavily; the 360 project's list of "21 missing PPC instructions" (update-form
loads/stores, `addc`/`addme`/`subfze`, `lvehx`, etc.) is a **ready-made checklist** of ops to
verify in ps3recomp's lifter for this binary.

## 4. Binary layout comparison

| | Xbox 360 (`default.xex`) | PS3 (`EBOOT.BIN`) |
|---|---|---|
| Container | XEX2 (LZX + AES-CBC) | SELF (NPDRM, key rev `0x16`) |
| Image base | `0x82000000` | segments @ `0x10000` (text), `0x180000` (data) |
| Entry point | `0x8214DB50` | `0x00186900` |
| `.text` | `0x820A0000`, size `0x237350` (~2.3 MB) | `0x10000`, filesz `0x1628E8` (~1.45 MB) |
| Data/BSS | `.data` @ `0x822E0000` | PT_LOAD2 @ `0x180000`, memsz `0xC1560` |
| Functions | **15,237** (XenonRecomp) | **14,754** (ps3recomp `find_functions`) — **96.8%** |
| ABI helpers | save/rest gpr/fpr/vmx @ `0x8225xxxx` | TBD (ps3recomp detects via OPD) |

The PS3 code section is smaller (~1.45 MB vs ~2.3 MB). Expected: the 360 image bundles more
XBLA/XAM/Live glue and a different CRT; the PS3 SNC/GCC toolchain emits tighter code. The
**arcade-core function cluster** should correspond closely between the two — useful for naming and
sanity-checking lifted PS3 functions against their 360 counterparts.

## 5. What transfers vs. what's new

### Transfers directly (lessons already paid for on the 360)
- **`.SR` archive format & load path** — same data, same parsing logic.
- **Arcade emulator core structure** — function-level behaviour is platform-agnostic.
- **VMX/AltiVec usage patterns** — same vector math, same lifting concerns.
- **Speed-fix insight** — the 360 build needed *timebase scaling* (guest 49.875 MHz `mftb`/`__rdtsc`
  mismatch) and a *precise frame limiter*. The PS3 PPE timebase (`mftb` @ 79.8 MHz / 80 MHz) has the
  exact same class of problem; ps3recomp will need the same treatment.
- **Menu / unlock logic** — reverse-engineered achievement/unlock overrides on the 360 map to PS3
  trophy (`NPWR01444_00`) and save logic.

### New work (the OS layer — exactly what ps3recomp provides)
| Concern | 360 (ReXGlue) | PS3 (ps3recomp) |
|---|---|---|
| Kernel | Xbox 360 kernel imports | LV2 syscalls + `sysPrxForUser` |
| GPU | Xenos → D3D12/Vulkan | RSX (NV47xx) → D3D12 (`cellGcmSys`) |
| Audio | XMA2 | `cellAudio` → WASAPI |
| Input | XInput | `cellPad` → XInput/SDL |
| Modules | static kernel lib | PRX/NID resolution |
| Container | XEX2 loader | SELF decrypt + ELF loader |

## 6. Concrete next actions this cross-ref unlocks

1. **Decrypt `EBOOT.BIN`** → `EBOOT.elf` (retail NPDRM, key rev `0x16`; via RPCS3 "Decrypt PS3
   binaries" or `ps3sce`/scetool with keys). The plaintext ELF header is already visible at file
   offset `0x90`, but segment payloads are encrypted.
2. **Run ps3recomp analysis** (`elf_parser.py` → `find_functions.py` → `ppu_disasm.py`); compare
   the function count and clustering against the 360's 15,237.
3. **Diff the import/NID set** against the `flOw` import map — Simpsons is a much smaller, simpler
   title (no PhyreEngine), so expect a *subset*: `cellGcmSys`, `cellAudio`, `cellPad`, `cellFs`,
   `cellSysutil`, `cellSysmodule`, `sysPrxForUser`, plus trophy/`sceNp` and `cellGame`.
4. **Port the 360 speed-fix early** — apply timebase scaling before chasing frame-rate bugs.
5. **Use the 360 lifted source as an oracle** — when a PS3 function misbehaves, find the matching
   360 function (same arcade core) to understand intended behaviour.
