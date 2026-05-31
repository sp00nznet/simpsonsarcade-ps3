/*
 * The Simpsons Arcade Game — HLE module dispatch
 *
 * The EBOOT imports 256 functions across 20 PS3 system libraries (see
 * imports.json). Each import is a 0x20-byte stub in the .text stub region
 * (0x150D34..0x152D14). simpsons_hle() is the single dispatch entry every
 * import funnels into; it routes the core libraries to the ps3recomp HLE
 * runtime and stubs the online/media libraries for offline play.
 *
 * NOTE (scaffold): direct `bl` calls to import stubs currently still hit the
 * lifted (placeholder) stub bodies in ppu_recomp.cpp. The next step is a
 * post-lift pass that rewrites those 207 lifted stub bodies to call
 * simpsons_hle() — see tools/ (mirrors flОw's post_lift.py). Indirect (bctrl)
 * import calls already route correctly via g_recompiled_funcs[].
 */
#include "recomp/ppu_recomp.h"
#include <ps3emu/module.h>
#include <cstdio>
#include <cstring>

/* Required by the ps3recomp runtime: each project owns one registry. */
ps3_module_registry g_ps3_module_registry = {};

/* Import metadata produced by tools/gen_runtime.py */
struct ImportEntry { uint32_t stub_addr; uint32_t nid; const char* lib; const char* name; };
extern "C" const ImportEntry g_imports[];
extern "C" const size_t      g_import_count;

/* Libraries we treat as no-ops for offline single-machine play. */
static bool is_stubbed_lib(const char* lib) {
    static const char* stubbed[] = {
        "sceNp", "sceNp2", "sceNpTrophy", "sceNpCommerce2", "cellNetCtl",
        "sys_net", "cellUserInfo", "cellSysutilAvc2", "cellSail", "cellAtrac",
    };
    for (const char* s : stubbed)
        if (std::strcmp(lib, s) == 0) return true;
    return false;
}

/* Single HLE dispatch entry. Returns via ctx->gpr[3]. */
extern "C" void simpsons_hle(uint32_t nid, ppu_context* ctx) {
    /* Locate metadata for diagnostics + routing. */
    const ImportEntry* e = nullptr;
    for (size_t i = 0; i < g_import_count; i++)
        if (g_imports[i].nid == nid) { e = &g_imports[i]; break; }

    const char* lib  = e ? e->lib  : "?";
    const char* name = (e && e->name[0]) ? e->name : "<nid>";

    static int logged = 0;
    if (logged++ < 200)
        fprintf(stderr, "[hle] %s::%s (nid=0x%08X)\n", lib, name, nid);

    if (e && is_stubbed_lib(lib)) {
        ctx->gpr[3] = 0; /* CELL_OK */
        return;
    }

    /* TODO: bridge core libraries (cellGcmSys, cellAudio, sys_io, sys_fs,
     * cellSysutil, cellSysmodule, sysPrxForUser, cellGame, cellRtc, cellSpurs)
     * to the ps3recomp module handlers. For now return CELL_OK so init paths
     * proceed; flesh these out as the boot trace demands them. */
    ctx->gpr[3] = 0;
}

/* ---- Guest-callback trampoline -----------------------------------------
 * The ps3recomp runtime invokes guest OPD callbacks (cellSysutil events,
 * save-data completion, GCM vblank handlers) through this hook. */
#include <ps3emu/guest_call.h>
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void ps3_indirect_call(ppu_context* ctx);

extern "C" void simpsons_guest_caller(uint32_t opd_addr,
                                      uint64_t a0, uint64_t a1,
                                      uint64_t a2, uint64_t a3) {
    if (!opd_addr) return;
    uint32_t func = vm_read32(opd_addr);
    uint32_t toc  = vm_read32(opd_addr + 4);
    if (!func) return;

    static uint32_t s_cb_sp = 0x0FF00000;
    ppu_context cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.cia = func; cb.ctr = func;
    cb.gpr[1] = s_cb_sp;
    cb.gpr[2] = toc;
    cb.gpr[3] = a0; cb.gpr[4] = a1; cb.gpr[5] = a2; cb.gpr[6] = a3;
    s_cb_sp -= 0x1000;
    if (s_cb_sp < 0x0FE00000) s_cb_sp = 0x0FF00000;
    ps3_indirect_call(&cb);
}
