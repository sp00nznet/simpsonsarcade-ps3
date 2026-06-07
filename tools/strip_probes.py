"""Cleanup pass: strip bring-up debug fprintf probes from the generated
src/recomp/ppu_recomp.cpp while PRESERVING the functional changes
(the in-thread 0xC1 clear in func_00130E24 + the in-thread counter
converge at loc_00130BDC that give determinism/boot progress)."""
import io

p = 'src/recomp/ppu_recomp.cpp'
with io.open(p, encoding='utf-8', errors='replace') as f:
    lines = f.read().split('\n')

out = []
i = 0
removed = 0
n = len(lines)
while i < n:
    ln = lines[i]

    # (A) BDC converge: a multi-line block whose TAIL is the fprintf; keep the
    #     functional converge, replace the fprintf tail with the closing brace.
    if 'fprintf(stderr,"[criQ] BDC converge' in ln:
        # this line + the next ("pa,vm_read32(pa),ca); }") -> just close block
        out.append('          }')
        i += 2
        removed += 1
        continue

    # (B) in-thread 0xC1 clear logging: drop the `static int o` + its fprintf,
    #     keep the surrounding clear logic.
    if '[cri-task] clearing task' in ln:
        # also drop the preceding `static int o = 0;` line if we just emitted it
        if out and out[-1].strip().startswith('static int o = 0;'):
            out.pop()
        i += 1
        removed += 1
        continue

    # (C) all other [criQ] probes are standalone debug blocks (1 or 2 lines).
    if 'fprintf(stderr,"[criQ]' in ln:
        if ln.rstrip().endswith('}'):
            i += 1            # single-line block
        else:
            i += 2            # block spans this line + continuation
        removed += 1
        continue

    out.append(ln)
    i += 1

with io.open(p, 'w', encoding='utf-8') as f:
    f.write('\n'.join(out))
print('removed', removed, 'probe blocks; remaining [criQ]:', '\n'.join(out).count('[criQ]'),
      'remaining [cri-task]:', '\n'.join(out).count('[cri-task]'))
