#!/usr/bin/env python3
"""
gen_runtime.py — generate the runtime bridge sources for the Simpsons port.

Post-lift step. Reconciles three sets of func_<addr> symbols in the big TU
(DECLARED in ppu_recomp.h, DEFINED in ppu_recomp.cpp, USED/called in it) and
wires the 256 import stubs to the HLE layer.

Outputs into src/recomp/:
  * (patches ppu_recomp.h)   — declarations for USED-but-undeclared targets +
                               a simpsons_hle() prototype.
  * (rewrites ppu_recomp.cpp)— redirects each LIFTED import-stub body to call
                               simpsons_hle(nid, ctx) (direct `bl` imports).
  * func_table.cpp           — g_recompiled_funcs[] over DECLARED ∪ USED.
  * import_stubs.cpp         — import metadata table + bodies for UNLIFTED
                               import stubs (also -> simpsons_hle).
  * missing_stubs.cpp        — empty bodies for remaining unlifted call targets.

Usage:  python tools/gen_runtime.py
"""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "src", "recomp", "ppu_recomp.h")
SOURCE = os.path.join(ROOT, "src", "recomp", "ppu_recomp.cpp")
IMPORTS = os.path.join(ROOT, "imports.json")
OUT_DIR = os.path.join(ROOT, "src", "recomp")

DECL_RE = re.compile(r"void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\);")
DEF_RE = re.compile(r"^void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\) \{")
TOK_RE = re.compile(r"func_[0-9A-Fa-f]{8}")
MARKER = "/* === gen_runtime.py: declarations for undeclared call targets === */"
CRI_GATE_MARKER = "/* gen_runtime: CRI task-completion gate */"
CRI_CONV_MARKER = "/* gen_runtime: CRI producer/consumer converge */"

# The guest's Dinkumware allocator relies on OS heap init we don't reproduce,
# so its malloc returns NULL and operator new throws std::bad_alloc during C++
# static construction. Redirect the malloc/free family (identified from the
# heap-handle wrappers at toc-0x3100) to the host bump allocator in
# malloc_override.cpp. Each maps a lifted wrapper -> the hle_guest_* entry that
# takes (ppu_context*) with the same r3-in/r3-out convention.
ALLOC_REDIRECTS = {
    # Redirect at the C++ operator level where the size is unambiguously r3.
    # (operator new internally calls the C malloc wrapper with size in r4, so
    # redirecting the C wrapper alone mis-sized allocations.)
    "func_00011DD8": "hle_guest_malloc",   # operator new(size)   [r3=size]
    "func_00011CAC": "hle_guest_malloc",   # operator new variant [r3=size] -> func_00022874
    "func_000227D8": "hle_guest_malloc",   # malloc(size)         [r3=size]
    "func_000227A8": "hle_guest_free",     # free(ptr)            [r3=ptr]
    "func_00022810": "hle_guest_realloc",  # realloc(ptr,size)    [r3=ptr,r4=size] -> func_000225C0
    "func_00022768": "hle_guest_malloc",   # aligned alloc        [r3=size]        -> func_0002249C
    "func_00022874": "hle_guest_malloc",   # malloc(size)         [r3=size] -> heap@*(toc-0x3100)
                                           # called DIRECTLY by func_001398B8 (the render
                                           # local-mem-allocator factory); unredirected it hit
                                           # the dead game heap -> NULL -> framebuffer alloc retry.
}
# The CRT heap is never OS-initialised, so every allocator that walks it returns
# NULL → std::bad_alloc. Each wrapper above loads the heap handle from
# *(toc-0x3100); the hle_guest_* targets match their r3/r4 ABI (realloc verified:
# func_000225C0 frees when size==0 else reallocs; aligned alloc takes size in r3,
# bump allocator is 16-byte aligned).


def declared_funcs():
    """Declarations the lifter emitted — excludes our own appended block so the
    step is idempotent across re-runs (else we'd strip+not-readd them)."""
    text = open(HEADER, "r", encoding="latin1").read()
    if MARKER in text:
        text = text[: text.index(MARKER)]
    return set(DECL_RE.findall(text))


def load_imports():
    """-> (list of dicts, {func_name: nid_int})."""
    imp = json.load(open(IMPORTS))
    nid_by_fn = {f"func_{int(x['stub'],16):08X}": int(x["nid"], 16) for x in imp}
    return imp, nid_by_fn


def scan_source():
    defined, used = set(), set()
    with open(SOURCE, "r", encoding="latin1") as f:
        for line in f:
            m = DEF_RE.match(line)
            if m:
                defined.add(m.group(1))
            if "func_" in line:
                used.update(TOK_RE.findall(line))
    return defined, used


def redirect_import_stubs(targets):
    """Replace each lifted import-stub body with a simpsons_hle() tail call.
    `targets` = {func_name: nid_int} restricted to LIFTED stubs."""
    lines = open(SOURCE, "r", encoding="latin1").read().split("\n")
    hdr = {f"void {fn}(ppu_context* ctx) {{": fn for fn in targets}
    out, i, n, done = [], 0, len(lines), 0
    while i < n:
        fn = hdr.get(lines[i].rstrip("\r"))
        if fn:
            j = i + 1
            while j < n and lines[j] != "}":
                j += 1
            out.append(lines[i])
            out.append(f"    simpsons_hle(0x{targets[fn]:08X}u, ctx);")
            out.append("}")
            i = j + 1
            done += 1
        else:
            out.append(lines[i])
            i += 1
    open(SOURCE, "w", encoding="latin1").write("\n".join(out))
    print(f"ppu_recomp.cpp: redirected {done} lifted import stubs -> simpsons_hle")


# Temporary: prepend an entry-log to these functions so we can see which one
# (in func_0006F238's try block) throws the std::bad_alloc during boot.
TRACE_FUNCS = {
    "func_0006F238", "func_0009E8D4", "func_000812E8", "func_00139B90",
    "func_0005A630", "func_00011CAC", "func_000ADD20", "func_00081468",
    "func_00080B98", "func_000B2C68",
}

def trace_functions():
    if not TRACE_FUNCS:
        return
    lines = open(SOURCE, "r", encoding="latin1").read().split("\n")
    hdr = {f"void {fn}(ppu_context* ctx) {{": fn for fn in TRACE_FUNCS}
    out, done = [], 0
    for ln in lines:
        out.append(ln)
        fn = hdr.get(ln.rstrip("\r"))
        if fn:
            out.append(f'    {{ static int _t=0; if(_t++<3) fprintf(stderr,"[trace] {fn}\\n"); }}')
            done += 1
    open(SOURCE, "w", encoding="latin1").write("\n".join(out))
    print(f"ppu_recomp.cpp: traced {done} functions")


def redirect_allocators():
    """Replace the lifted malloc/free wrapper bodies with a tail call to the
    host bump allocator (hle_guest_*). Same body-rewrite as the import-stub
    redirect, but the target is a named C function rather than simpsons_hle."""
    lines = open(SOURCE, "r", encoding="latin1").read().split("\n")
    hdr = {f"void {fn}(ppu_context* ctx) {{": fn for fn in ALLOC_REDIRECTS}
    out, i, n, done = [], 0, len(lines), 0
    while i < n:
        fn = hdr.get(lines[i].rstrip("\r"))
        if fn:
            j = i + 1
            while j < n and lines[j] != "}":
                j += 1
            out.append(lines[i])
            out.append(f"    {ALLOC_REDIRECTS[fn]}(ctx); return;")
            out.append("}")
            i = j + 1
            done += 1
        else:
            out.append(lines[i]); i += 1
    open(SOURCE, "w", encoding="latin1").write("\n".join(out))
    print(f"ppu_recomp.cpp: redirected {done} allocator(s) -> hle_guest_*")


def patch_cri_completion():
    """Re-apply the in-thread CRI task-completion primitives that let crCommandQueue
    reach completion without the (not-yet-run) SPU consumer, so the boot is
    deterministic and reaches render init. The lifter emits clean func_00130E24 /
    loc_00130BDC; these inserts are dropped by a re-lift, so we reapply them here.

    Two primitives:
      (1) func_00130E24 — clear the per-task 0xC1 busy halfword (task+8) at entry,
          so the task-completion gate passes (-> func_00130CB0 posts completion).
      (2) loc_00130BDC  — converge the producer/consumer counters ([gpr28]==[gpr27])
          in-thread at the exact check site (no cross-thread race).

    Idempotent: skips if either the marker OR the primitive's functional signature
    is already present (handles both a fresh re-lift and the legacy hand-edits)."""
    text = open(SOURCE, "r", encoding="latin1").read()
    have_gate = (CRI_GATE_MARKER in text or
                 "vm_write32(task + 8, vm_read32(task + 8) & 0x0000FFFFu)" in text)
    have_conv = (CRI_CONV_MARKER in text or
                 "if (vm_read32(ca)!=pv) vm_write32(ca, pv)" in text)
    if have_gate and have_conv:
        print("ppu_recomp.cpp: CRI completion primitives already present — skipped")
        return
    gate_lines = [
        "        " + CRI_GATE_MARKER,
        "        { uint32_t task = (uint32_t)ctx->gpr[9];",
        "          if (task >= 0x10000 && task < 0x10000000 && (vm_read32(task + 8) >> 16) == 0xC1)",
        "              vm_write32(task + 8, vm_read32(task + 8) & 0x0000FFFFu); }",
    ]
    conv_lines = [
        "        " + CRI_CONV_MARKER,
        "        { uint32_t pa=(uint32_t)ctx->gpr[28], ca=(uint32_t)ctx->gpr[27];",
        "          if (pa>=0x10000 && pa<0x10000000 && ca>=0x10000 && ca<0x10000000) {",
        "              uint32_t pv=vm_read32(pa); if (vm_read32(ca)!=pv) vm_write32(ca, pv); } }",
    ]
    out, n_gate, n_conv = [], 0, 0
    for ln in text.split("\n"):
        out.append(ln)
        s = ln.rstrip("\r")
        if not have_gate and s == "void func_00130E24(ppu_context* ctx) {":
            out.extend(gate_lines); n_gate += 1
        elif not have_conv and s == "loc_00130BDC:":
            out.extend(conv_lines); n_conv += 1
    open(SOURCE, "w", encoding="latin1").write("\n".join(out))
    print(f"ppu_recomp.cpp: CRI completion — inserted gate={n_gate}, converge sites={n_conv}")


def patch_header(undeclared):
    text = open(HEADER, "r", encoding="latin1").read()
    if MARKER in text:
        text = text[: text.index(MARKER)].rstrip() + "\n"
    lines = [MARKER, '#ifdef __cplusplus', 'extern "C" {', "#endif",
             "void simpsons_hle(unsigned int nid, ppu_context* ctx);",
             "void hle_guest_malloc(ppu_context* ctx);",
             "void hle_guest_free(ppu_context* ctx);",
             "void hle_guest_realloc(ppu_context* ctx);",
             "void hle_guest_calloc(ppu_context* ctx);"]
    lines += [f"void {fn}(ppu_context* ctx);" for fn in sorted(undeclared)]
    lines += ["#ifdef __cplusplus", "}", "#endif", ""]
    open(HEADER, "w", encoding="latin1").write(text + "\n" + "\n".join(lines))
    print(f"ppu_recomp.h: +{len(undeclared)} decls + simpsons_hle prototype")


def gen_func_table(all_funcs):
    with open(os.path.join(OUT_DIR, "func_table.cpp"), "w", encoding="utf-8") as f:
        f.write('/* Auto-generated by tools/gen_runtime.py — do not edit. */\n')
        f.write('#include "ppu_recomp.h"\n#include <cstddef>\n#include <cstdint>\n\n')
        f.write('struct RecompiledFunc { uint32_t guest_addr; '
                'void (*host_func)(void* ctx); const char* name; };\n\n')
        f.write('extern "C" const RecompiledFunc g_recompiled_funcs[] = {\n')
        for fn in sorted(all_funcs):
            f.write(f'    {{ 0x{int(fn[5:],16):08X}, (void(*)(void*))&{fn}, "{fn}" }},\n')
        f.write('};\n')
        f.write(f'extern "C" const size_t g_recompiled_func_count = {len(all_funcs)};\n')
    print(f"func_table.cpp: {len(all_funcs)} functions")


def gen_import_stubs(imp, unlifted_nid):
    """Metadata table for all imports + func bodies for UNLIFTED import stubs."""
    with open(os.path.join(OUT_DIR, "import_stubs.cpp"), "w", encoding="utf-8") as f:
        f.write('/* Auto-generated by tools/gen_runtime.py — do not edit. */\n')
        f.write('#include "ppu_recomp.h"\n#include <cstdint>\n#include <cstddef>\n\n')
        f.write('struct ImportEntry { uint32_t stub_addr; uint32_t nid; '
                'const char* lib; const char* name; };\n\n')
        f.write('extern "C" const ImportEntry g_imports[] = {\n')
        for x in imp:
            f.write(f'    {{ 0x{int(x["stub"],16):08X}, 0x{int(x["nid"],16):08X}, '
                    f'"{x["lib"]}", "{x.get("name","")}" }},\n')
        f.write('};\n')
        f.write(f'extern "C" const size_t g_import_count = {len(imp)};\n\n')
        f.write('/* Bodies for import stubs the lifter did not emit. */\n')
        for fn, nid in sorted(unlifted_nid.items()):
            f.write(f'void {fn}(ppu_context* ctx) {{ simpsons_hle(0x{nid:08X}u, ctx); }}\n')
    print(f"import_stubs.cpp: {len(imp)} imports, {len(unlifted_nid)} unlifted bodies")


def gen_missing_stubs(missing):
    with open(os.path.join(OUT_DIR, "missing_stubs.cpp"), "w", encoding="utf-8") as f:
        f.write('/* Auto-generated by tools/gen_runtime.py — do not edit. */\n')
        f.write('#include "ppu_recomp.h"\n#include <cstdio>\n\n')
        for fn in sorted(missing):
            f.write(f'void {fn}(ppu_context* ctx) {{ (void)ctx; static int w=0; '
                    f'if(!w++) fprintf(stderr,"[stub] {fn} (unlifted)\\n"); }}\n')
    print(f"missing_stubs.cpp: {len(missing)} unlifted call targets")


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    imp, nid_by_fn = load_imports()
    declared = declared_funcs()
    defined, used = scan_source()

    import_fns = set(nid_by_fn)
    lifted_imports = {fn: nid_by_fn[fn] for fn in import_fns & defined}
    unlifted_imports = {fn: nid_by_fn[fn] for fn in import_fns - defined}

    undeclared = used - declared
    all_funcs = declared | used
    # stub bodies: everything used/declared but not defined, minus the unlifted
    # imports (defined in import_stubs.cpp) — avoids duplicate symbols.
    missing = (all_funcs - defined) - set(unlifted_imports)

    print(f"declared={len(declared)} defined={len(defined)} used={len(used)} "
          f"imports={len(import_fns)} (lifted={len(lifted_imports)} "
          f"unlifted={len(unlifted_imports)}) undeclared={len(undeclared)} stubs={len(missing)}")

    patch_header(undeclared)
    redirect_import_stubs(lifted_imports)
    redirect_allocators()
    patch_cri_completion()
    # trace_functions()  # (diagnostic; re-enable to trace specific funcs)
    gen_func_table(all_funcs)
    gen_import_stubs(imp, unlifted_imports)
    gen_missing_stubs(missing)
    print("done")
