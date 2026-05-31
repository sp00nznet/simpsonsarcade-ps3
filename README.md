# 🍩 simpsonsarcade-ps3 — Static Recompilation

> *"Mmm... native code."* — Homer J. Simpson, probably

A static recompilation of **The Simpsons Arcade Game** (PlayStation 3 / PSN) into a native
PC executable — no emulator required — built on [ps3recomp](https://github.com/sp00nznet/ps3recomp).

This is the PS3 sibling of our **already-playable** Xbox 360 port,
[`simpsonsarcade`](https://github.com/sp00nznet/simpsonsarcade). Same game, same Konami arcade
core, same data files — just a different flavour of PowerPC underneath. The 360 build already
boots through menus, renders gameplay, plays audio, and takes controller input at full speed.
The plan: steal everything we learned over there and point it at the Cell.

> **Why this is easier than it sounds:** both the Xbox 360 (Xenon) and the PS3 (Cell PPE) run
> **64-bit big-endian PowerPC with VMX/AltiVec**. We already recompiled this exact game once.
> The PS3 EBOOT is *the same game logic* compiled by a different toolchain for a sister CPU.

---

## 📺 The Game

| | |
|---|---|
| **Title** | The Simpsons Arcade Game |
| **Platform** | PlayStation 3 (PSN download) |
| **Title ID** | `NPUB30563` |
| **Content ID** | `UP0101-NPUB30563_00-SIMPSONSARCADEKO` |
| **Trophy ID** | `NPWR01444_00` |
| **Developer** | Backbone Entertainment / Konami |
| **Publisher** | Konami |
| **Original** | 1991 Konami 4-player arcade beat-'em-up |
| **Players** | Homer, Marge, Bart & Lisa (1–4 player co-op) |

Under the hood, the PSN/XBLA release is an **arcade-emulator wrapper**: the original 1991 coin-op
ROM lives inside the `SIMPSONS.SR` data archive (we can see the romset manifest —
`Simpsons_4J.KON`, `..._TILES/SPRITES/SOUND/SAMPLES.ROM`), and the EBOOT emulates the arcade
hardware around it — Konami 052001 (6809) + Z80 + `YM2151` + K053260, all named right in the
binary. Same architectural trick as the Genesis-on-XBLA ports like Comix Zone. **We recompile the
emulator; the game's 6809 code just gets interpreted from the ROM** — which is why the
already-working 360 build is such a strong oracle. See [`docs/emulator-architecture.md`](docs/emulator-architecture.md).

---

## 🎯 Status

| Milestone | Status |
|---|---|
| Locate & extract the PSN PKG | ✅ Done |
| Confirm it's the same game as the 360 build | ✅ Done — `SIMPSONS.SR` is **byte-identical** (2,748,416 B) |
| ELF / SELF structural analysis | ✅ Done — PPC64 BE, entry `0x186900`, text @ `0x10000` |
| 360 ↔ PS3 cross-reference | ✅ Done — see [`docs/360-crossref.md`](docs/360-crossref.md) |
| Decrypt EBOOT.BIN (SELF → ELF) | ✅ Done — `rpcs3 --decrypt` (no RAP needed) |
| Function discovery | ✅ Done — **14,754 functions** (vs 360's 15,237 — 96.8%) |
| It's an arcade emulator! | ✅ Confirmed — ROM names, `YM2151`, `z80`, sprite engine in the EBOOT |
| Import / NID resolution | ✅ Done — **20 libs, 256 funcs** (`imports.json`) |
| PPU lift (ppu_lifter → C++) | ✅ Done — **17,397 funcs, 119 MB** |
| Instruction coverage | ✅ Done — patched lifter; **0 real-instruction TODOs** remain |
| Runtime link against ps3recomp | 🔜 Next |
| Reach `main()` / CRT init | ⬜ Not started |
| Arcade core running | ⬜ Not started |
| Graphics (RSX → D3D12) | ⬜ Not started |
| Audio / Input | ⬜ Not started |
| 🍩 Playable | ⬜ Not started |

See [`PROGRESS.md`](PROGRESS.md) for the blow-by-blow.

---

## 🧬 How It Lines Up With the 360 Build

The whole reason this project exists: **we already did the hard part once.** Here's the overlap.

| | Xbox 360 (done, playable) | PlayStation 3 (this repo) |
|---|---|---|
| CPU | Xenon — 64-bit big-endian PowerPC + VMX128 | Cell PPE — 64-bit big-endian PowerPC + VMX/AltiVec |
| Executable | `default.xex` (XEX2, 1.33 MB) | `EBOOT.BIN` (SELF, 1.63 MB) |
| Code base | `.text` @ `0x820A0000`, size `0x237350` | `.text` @ `0x10000`, size `0x1628E8` |
| Entry point | `0x8214DB50` | `0x00186900` |
| Recompiler | XenonRecomp → C++ | ps3recomp `ppu_lifter` → C++ |
| Runtime | ReXGlue SDK (Xbox 360 HLE) | ps3recomp HLE libs (LV2/Cell HLE) |
| **Game data** | `0B/SIMPSONS.SR` (2,748,416 B) | `0B/SIMPSONS.SR` (**2,748,416 B — identical**) |
| **Assets** | `0B/SIMPSONS_FW.SR` (14.4 MB) | `0B/SIMPSONS_FW.SR` (15.8 MB) |

**What transfers directly:** the function-level structure of the arcade emulator core, the `.SR`
archive format and load path, the VMX/AltiVec usage patterns, the speed-fix lessons (timebase
scaling, frame limiting), and the menu/unlock logic we reverse-engineered on the 360.

**What's different:** the OS layer (Xbox kernel/XAM/XMA vs. LV2/Cell/cellGcm/cellAudio), the GPU
(Xenos vs. RSX), the executable container, and address layout. Those are exactly the gaps
ps3recomp already fills (see the `flOw` and `tokyojungle` ports).

Full analysis: [`docs/360-crossref.md`](docs/360-crossref.md) · PS3 binary notes:
[`docs/binary-analysis.md`](docs/binary-analysis.md).

---

## 🛠️ Pipeline

```
   PSN PKG  ──►  EBOOT.BIN (SELF)  ──►  EBOOT.elf  ──►  ppu_lifter  ──►  C++  ──►  link ps3recomp  ──►  simpsons.exe
   (done)        (encrypted, next)      (decrypt)      (90k+ funcs)            (HLE LV2/RSX/audio)
```

Same five-stage flow as every ps3recomp port — extract, decrypt, analyse, lift, link. The
[`flOw`](https://github.com/sp00nznet/flow) port is the reference implementation of this pipeline.

## 📦 Building (once lifting begins)

```bash
# Prereqs: Python 3.8+, CMake 3.20+, MSVC 2022 (or Clang/GCC), ps3recomp checked out at ../../ps3
# 1. Decrypt your own legally-dumped EBOOT.BIN → game/EBOOT.elf
# 2. Analyse + lift
python tools/recompile.py --config config.toml --ps3recomp-dir ../../ps3
# 3. Build
cmake -B build -DPS3RECOMP_DIR=../../ps3
cmake --build build --config Release
```

## ⚖️ Legal

This repository contains **no copyrighted game code, assets, binaries, or encryption keys** —
only analysis notes, configuration, and recompilation tooling. You must supply your own legally
obtained copy of The Simpsons Arcade Game. `pkg/`, `extracted/`, and `game/` are git-ignored.

## 🔗 Related Projects

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) — the PS3 HLE runtime this links against
- [simpsonsarcade](https://github.com/sp00nznet/simpsonsarcade) — **the 360 port of this same game (playable)**
- [flOw](https://github.com/sp00nznet/flow) · [tokyojungle](https://github.com/sp00nznet/tokyojungle) — sister PS3 ports
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) · [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) — prior art
- [RPCS3](https://github.com/RPCS3/rpcs3) — emulator whose HLE research makes this possible

---

*"I'm not normally a praying man, but if you're up there, please save me, Superman."* — port it natively instead.
