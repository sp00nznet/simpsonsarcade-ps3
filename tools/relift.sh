#!/bin/sh
# Regenerate everything git-ignored: the lifted PPU tree and the HLE NID table.
# Run from the repo root. PS3RECOMP defaults to the sibling checkout.
set -e
PS3RECOMP="${PS3RECOMP:-../ps3recomp}"

# 0x152D34 is the end of the last executable section (the .lib.stub import
# trampolines at 0x150D34+0x2000); past it is .rodata, which the branch-target
# pass would otherwise explode into bogus functions.
python "$PS3RECOMP/tools/find_functions.py" game/EBOOT.elf --output analysis/functions.json

# --hle-stubs rewrites each import trampoline as ps3_hle_call(nid), so direct
# `bl` calls to an import dispatch to the HLE handler instead of running the
# literal stub (whose pointer table the recomp never fills).
rm -rf src/recomp && mkdir -p src/recomp src/gen
python "$PS3RECOMP/tools/ppu_lifter.py" game/EBOOT.elf \
    --functions analysis/functions.json \
    --hle-stubs imports.json \
    --code-end 0x152D34 \
    -o src/recomp

python "$PS3RECOMP/tools/gen_hle_nids.py" --all --out src/gen/ppu_hle_nids.cpp
