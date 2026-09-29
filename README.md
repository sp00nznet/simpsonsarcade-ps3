# 🍩 simpsonsarcade-ps3

> *"Mmm... native code."* — Homer J. Simpson, probably

A static recompilation of **The Simpsons Arcade Game** (PlayStation 3 / PSN) into a native
PC executable, built on [ps3recomp](https://github.com/sp00nznet/ps3recomp). No emulator.

It is playable: intro, menus, saves, and the arcade game itself, with keyboard or pad. It also
plays **online** through [psnr](https://github.com/sp00nznet/psnr), a stand-in for PSN's
matchmaking.

![The Simpsons Arcade Game running natively](docs/media/hero.gif)

*Main menu → profile dialog → character select → Stage 1, captured from the native build.*

| | |
|---|---|
| ![Main menu](docs/media/04-main-menu.png) | ![Profile dialog](docs/media/05-profile-dialog.png) |
| ![Stage 1](docs/media/06-stage1.png) | ![Intro](docs/media/01-intro.png) |

## Status

| | |
|---|---|
| Boot, intro, menus, attract mode | ✅ |
| Gameplay (arcade core) | ✅ Stage 1 onward |
| Input | ✅ Keyboard; XInput pad when one is attached |
| Save / load | ✅ Settings and progress persist across boots |
| Graphics | ✅ RSX → D3D12 (`RSX_LIVE_DRAW=1`); no flash frames or garbled glyphs |
| Audio | ✅ Music (ATRAC3plus) and effects |
| Frame rate | ⚠️ 60 in menus, 35–57 in gameplay |
| **Online multiplayer (psnr)** | ✅ Create Match / Quick Match, lobby, co-op in sync |
| Online leaderboards | ❓ Not yet tried against psnr |

### Known issues

- **In-stage sound thins out when the frame rate drops.** The arcade core makes its sound per
  emulated frame, so below 60 fps its own sample stream has gaps.
- **Offline, the title shows its "sign in to post scores" notice.** That's expected without psnr.
  It doesn't block saving.

The full history, with measurements, is in [`PROGRESS.md`](PROGRESS.md).

## Online play (psnr)

The title's online mode works over [psnr](https://github.com/sp00nznet/psnr). psnr runs the
rooms; once players are matched, game traffic goes directly between them.

Tested: two players, both instances on one machine. Create Match, Quick Match, the online
lobby, character select, and the host's game setup transfer all work. Stage 1 then plays in sync
on both, with each player's input showing on both screens. The game allows up to four players;
three and four haven't been tried, and neither has play across two machines.

**Requirements.** Online needs ps3recomp changes that are still in review:
[#200](https://github.com/sp00nznet/ps3recomp/pull/200) (NP matchmaking over psnr, and P2P
sockets) and [#202](https://github.com/sp00nznet/ps3recomp/pull/202) (a lifter fix: without it,
the title's zlib corrupts the game setup it sends). Until they merge, build against a ps3recomp
checkout with both applied, and re-lift (`tools/relift.sh`).

**Two players on one machine:**

```bash
psnr                      # from the psnr repo; listens on :36100

# player 1 (hosts: Online Game → Create Match)
PS3_NET_ONLINE=1 PSNR_SERVER=127.0.0.1 PS3_NP_ONLINE_ID=homer PS3_NET_P2P_PORT=3658 \
PS3_VERBOSE=0 RSX_LIVE_DRAW=1 ./build/simpsons vfs/PS3_GAME/USRDIR/EBOOT.elf

# player 2 (joins: Online Game → Quick Match)
PS3_NET_ONLINE=1 PSNR_SERVER=127.0.0.1 PS3_NP_ONLINE_ID=bart  PS3_NET_P2P_PORT=3659 \
PS3_VERBOSE=0 RSX_LIVE_DRAW=1 ./build/simpsons vfs/PS3_GAME/USRDIR/EBOOT.elf
```

| Variable | Why |
|---|---|
| `PS3_NET_ONLINE=1`, `PSNR_SERVER` | Turn on real sockets and point at the psnr server. |
| `PS3_NP_ONLINE_ID` | The player's name. It also gives the instance its own console identity, which the game uses to tell players apart. |
| `PS3_NET_P2P_PORT` | The port other players reach this one on. Each instance on a machine needs its own. Across machines, open it to the other players. |
| `PS3_VERBOSE=0` | Required. With stderr redirected, the runtime otherwise logs every wait, which drops gameplay to about 1 fps, and the game then drops the other player as "not responding". |

Without these variables the game is offline, exactly as before.

## Building

Prereqs: Python 3.9+, CMake 3.20+, **clang-cl** + Ninja, and a sibling
[ps3recomp](https://github.com/sp00nznet/ps3recomp) checkout.

```bash
# 1. Decrypt your own legally obtained EBOOT.BIN to an ELF and lay out the game data:
#       vfs/PS3_GAME/PARAM.SFO
#       vfs/PS3_GAME/USRDIR/EBOOT.elf
#       vfs/PS3_GAME/USRDIR/0B/SIMPSONS.SR, SIMPSONS_FW.SR

# 2. Lift the PPU image and generate the HLE NID table:
PS3RECOMP=../ps3recomp ./tools/relift.sh

# 3. Capture the SPU job binaries (the game builds them in memory, so they only exist
#    when cellSpurs dispatches them), then lift again to include them:
SPU_DUMP_MISS=spu_dump ./build/simpsons vfs/PS3_GAME/USRDIR/EBOOT.elf
PS3RECOMP=../ps3recomp ./tools/relift.sh

# 4. Build (Release is the default; a debug build runs at a third of the speed):
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build

# 5. Run:
RSX_LIVE_DRAW=1 ./build/simpsons vfs/PS3_GAME/USRDIR/EBOOT.elf
```

**Controls (keyboard).** Arrows move · `Z` attack · `X` jump · `A`/`S` square/triangle ·
`Q`/`W` L1/R1 · `Enter` START · `Tab` SELECT. An XInput pad takes over when one is plugged in.

**Scripted runs.** `PAD_SCRIPT="15:0x0008,30:0x4000,..."` presses buttons at set frames, and
`LD_FRAME_DUMP=shots LD_FRAME_DUMP_EVERY=60` saves frames. Coin in with START (`0x0008`) once,
then use only CROSS (`0x4000`); START again opens the pause menu.

## The game

| | |
|---|---|
| **Title** | The Simpsons Arcade Game (PSN) |
| **Title ID** | `NPUB30563` |
| **Developer / Publisher** | Backbone Entertainment / Konami |
| **Original** | Konami's 1991 four-player arcade beat-'em-up |

The PSN release is an arcade emulator wrapped around the original 1991 ROM (inside
`SIMPSONS.SR`): the EBOOT emulates the Konami 052001, Z80, YM2151 and K053260. So this project
recompiles the emulator, and the game's own 6809 code is interpreted from the ROM. See
[`docs/emulator-architecture.md`](docs/emulator-architecture.md).

It is the same game as the Xbox 360 release, with a byte-identical `SIMPSONS.SR`, and a sibling
of the playable 360 port [`simpsonsarcade`](https://github.com/sp00nznet/simpsonsarcade). Both
CPUs are 64-bit big-endian PowerPC with VMX, so the 360 port served as a reference throughout.
See [`docs/360-crossref.md`](docs/360-crossref.md) and
[`docs/binary-analysis.md`](docs/binary-analysis.md).

## Legal

This repository contains **no copyrighted game code, assets, binaries, or encryption keys**, only
analysis notes, configuration and build tooling. You need your own legally obtained copy of the
game. This project is not affiliated with Konami, Backbone, Sony or Fox.

## Related projects

- [ps3recomp](https://github.com/sp00nznet/ps3recomp): the PS3 runtime this links against
- [psnr](https://github.com/sp00nznet/psnr): the online server
- [simpsonsarcade](https://github.com/sp00nznet/simpsonsarcade): the Xbox 360 port of the same game
- [flOw](https://github.com/sp00nznet/flow) · [tokyojungle](https://github.com/sp00nznet/tokyojungle): sister PS3 ports
- [RPCS3](https://github.com/RPCS3/rpcs3): the emulator whose HLE research makes this possible
