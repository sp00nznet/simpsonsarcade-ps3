# ps3recomp toolchain patches

Patches applied to the shared [ps3recomp](https://github.com/sp00nznet/ps3recomp) tools while
porting The Simpsons Arcade Game. These are generic PowerPC fixes — they benefit every ps3recomp
port (flОw, Tokyo Jungle, …), so they should be upstreamed.

## `ps3recomp-missing-ppc-instructions.patch`

The first lift of the Simpsons EBOOT left ~556 real-instruction TODOs across a handful of opcodes
(the rest of the 4,784 TODOs were opcode-0 data words misclassified as code). This patch teaches
`ppu_disasm.py` to decode them and `ppu_lifter.py` to emit C for them.

**Disassembler (`ppu_disasm.py`)**
- `mulhdu` — op31 xo=9 (was emitted as `op31_x9`).
- `stfiwx` — op31 xo=983, store float-as-integer-word (was `op31_x983`, 532×).
- **Bug fix:** op63 FP conversions were mis-tabled — `846→fctid, 847→fctidz, 814→fcfid`. The
  correct PowerPC encodings are `814→fctid, 815→fctidz, 846→fcfid`. This both recovered the
  missing `fctidz` (815, 19×) **and** corrected silently-swapped `fctid`/`fcfid` semantics.
- `frsqrte` (op63 xo=26), `fre` (op63 xo=24), `frsqrtes` (op59 xo=26) with their `frD, frB` form.

**Lifter (`ppu_lifter.py`)**
- `cntlzd` — 64-bit count-leading-zeros (`__builtin_clzll`, 64 for zero input).
- `stfiwx` — store low 32 bits of the FPR's raw contents.
- `fctiw` — added alongside the existing `fctiwz`.
- `frsqrte`/`frsqrtes` → `1.0/sqrt(x)`; `fre`/`fres` → `1.0/x`.

Regenerate from a clean ps3recomp checkout with:
```bash
cd /path/to/ps3recomp && git apply /path/to/ps3recomp-missing-ppc-instructions.patch
```
