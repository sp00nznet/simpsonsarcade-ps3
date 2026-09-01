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

# ---- SPU -------------------------------------------------------------------
# The title's SPURS job binaries are RAW images it builds in main memory, not
# ELFs embedded in the EBOOT, so extract_spu_images.py cannot find them. The
# only place their bytes exist is the moment cellSpurs hands them to the job
# dispatcher, so capture them from a run:
#
#   SPU_DUMP_MISS=spu_dump ./build/simpsons vfs/PS3_GAME/USRDIR/EBOOT.elf
#
# which writes one file per unrecognised fingerprint. Re-lift them here. The
# fingerprints in src/spu_workloads.c must match the filenames; if a capture
# yields a different fingerprint, update the registrations to match.
for bin in spu_dump/spujob_3DAC72B6B208C7C0_41984.bin:crjob \
           spu_dump/spujob_84CC39BF3EF23DA3_816.bin:crlist; do
    src="${bin%%:*}"; pfx="${bin##*:}"
    [ -f "$src" ] || { echo "missing $src -- capture with SPU_DUMP_MISS first"; continue; }
    # A job image is loaded at LS 0 and entered at its first instruction, so
    # --base 0 keeps the lifted addresses equal to the link-time ones.
    python "$PS3RECOMP/tools/find_spu_functions.py" "$src" --raw --base 0 \
        --out "spu_dump/${pfx}_funcs.json"
    rm -rf "src/spu_gen/$pfx" && mkdir -p "src/spu_gen/$pfx"
    python "$PS3RECOMP/tools/spu_lifter.py" "$src" --base 0 \
        --functions "spu_dump/${pfx}_funcs.json" \
        --symbol-prefix "${pfx}_" -o "src/spu_gen/$pfx"
done
