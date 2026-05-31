# The EBOOT Is an Arcade Emulator (read this before disassembling)

**Key mental model:** the PS3 `EBOOT.elf` is *not* a hand-written port of The Simpsons. It is a
**Konami arcade emulator** that loads the original 1991 coin-op ROM set and emulates the arcade
hardware around it. This is the same "SEGA Vintage Collection" / M2-style wrapper pattern used by
Backbone for these XBLA/PSN classic re-releases (cf. the Comix Zone Genesis-emulator XBLA port).

When we statically recompile the EBOOT, **we are recompiling the emulator host**. The actual game
logic is 6809 machine code living inside the ROM and is *interpreted by the recompiled emulator* —
we never have to understand or translate it. That's why this is tractable, and why the 360 build
(same emulator) is such a strong oracle.

## Evidence (strings found in the decrypted EBOOT)

| String | Meaning |
|---|---|
| `0B/%s.SR`, `0B\%s_FW.SR` | the archive load path (same on PS3 & 360) |
| `Simpsons_4J.KON`, `Simpsons_4Wa.KON` | main-CPU program ROMs (Konami 052001 / 6809) |
| `Simpsons_SOUND.ROM` | sound-CPU ROM (Z80) |
| `Simpsons_SAMPLES.ROM` | PCM sample ROM (K053260) |
| `Simpsons_SPRITES.ROM` | sprite ROM (K053247) |
| `Simpsons_TILES.ROM` | tile/background ROM (K052109) |
| `YM2151` | Yamaha FM synthesis chip (arcade music) |
| `z80` | Z80 sound CPU core |
| `DUPLICATE_SPRITE`, `REMOVE_SPRITE` | sprite-engine bookkeeping |
| `state.color0/1`, `Multi-sample 4x rotated` | renderer state (emulator video backend) |

## The romset (`SIMPSONS.SR` manifest, byte-identical to the 360 build)

```
offset      size       name
       0    256379     Simpsons_4J.KON.QZ        main program ROM
  258048    252893     Simpsons_4WA.KON.QZ       program ROM
  512000     51735     Simpsons_SOUND.ROM.QZ     Z80 sound program
  565248    976818     Simpsons_SAMPLES.ROM.QZ   K053260 PCM samples
 1542144    871810     Simpsons_SPRITES.ROM.QZ   K053247 sprites
 2414592    329921     Simpsons_TILES.ROM.QZ     K052109 tiles
```
`.QZ` = a compressed payload; the wrapper decompresses each ROM into emulated address space.
`SIMPSONS_FW.SR` holds the platform-specific front-end assets (UI, movies, audio).

## Original arcade hardware being emulated (Konami "Simpsons" 1991)

- **Main CPU:** Konami 052001 (custom 6809) — runs `*.KON`
- **Sound CPU:** Z80 — runs `Simpsons_SOUND.ROM`
- **Sound chips:** YM2151 (FM) + K053260 (PCM, uses `SAMPLES`)
- **Video:** K052109 (tilemaps) + K053247/K051960 (sprites) + K053251 (priority mixer)

## What this means for the recompilation

1. **Disassembly strategy** — look for emulator structures, not game code:
   - a **CPU core dispatch loop** (opcode decode → handler) for the 6809 and Z80,
   - a **guest memory map** / bank-switch tables,
   - **ROM load + `.QZ` decompression** routines (keyed off the `Simpsons_*.ROM` strings),
   - **device emulation**: tilemap renderer, sprite list builder (`*_SPRITE`), YM2151/K053260 mixers,
   - a **per-frame step** that runs N cycles of each CPU then composites video + audio.
2. **The 360 build is the oracle** — the same emulator was already recompiled and runs. Match PS3
   functions to their 360 counterparts to recover intent and names.
3. **Runtime needs are modest** — the emulator outputs a framebuffer (→ `cellGcmSys`/RSX) and a PCM
   stream (→ `cellAudio`), reads a pad (→ `sys_io`/cellPad), and loads `.SR` files (→ `sys_fs`).
   Everything else (`sceNp*`, `sys_net`, `cellSail`, `cellAtrac`) is front-end/online and can be
   stubbed for a first playable.
4. **Performance** — emulating a 6809 + Z80 + sound chips natively is trivial for a modern CPU;
   no SPU offload is required for the core (any `cellSpurs` use is likely incidental).

See [`imports.json`](../imports.json) for the resolved import table and
[`360-crossref.md`](360-crossref.md) for the platform overlap.
