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
#include "libs/system/sysPrxForUser.h"   /* runtime lwmutex/thread primitives */
#include <cstdio>
#include <cstring>

/* Required by the ps3recomp runtime: each project owns one registry. */
ps3_module_registry g_ps3_module_registry = {};

/* ---- sysPrxForUser sync bridges ------------------------------------------
 * The guest's userland lwmutex_lock does an atomic fast-path on the mutex's
 * owner field (high 32 bits of lock_var) and only traps to the kernel (this
 * HLE) on contention. If the HLE returns without updating the owner field,
 * the userland re-checks, still sees "not mine", and traps again — an
 * infinite lock/unlock/get_id spin. We back each guest lwmutex with a real
 * host CriticalSection (runtime sys_lwmutex_*, keyed by sleep_queue) and,
 * crucially, write the owner thread-id (0x10000, matching what
 * sys_ppu_thread_get_id returns) into lock_var on a successful lock so the
 * userland fast-path sees `owner == my_tid` and proceeds. (flОw approach.)
 *
 * Guest sys_lwmutex_t layout (big-endian): +0 lock_var(u64) {owner,waiter},
 * +8 attribute, +12 recursive_count, +16 sleep_queue (host slot id), +20 pad.
 */
static const uint32_t HLE_MAIN_TID = 0x10000;

static void lwmutex_read(uint32_t a, sys_lwmutex_t_hle* m) {
    m->lock_var        = vm_read64(a);
    m->attribute       = vm_read32(a + 8);
    m->recursive_count = vm_read32(a + 12);
    m->sleep_queue     = vm_read32(a + 16);
    m->pad             = vm_read32(a + 20);
}
static void lwmutex_write(uint32_t a, const sys_lwmutex_t_hle* m) {
    vm_write64(a,      m->lock_var);
    vm_write32(a + 8,  m->attribute);
    vm_write32(a + 12, m->recursive_count);
    vm_write32(a + 16, m->sleep_queue);
    vm_write32(a + 20, m->pad);
}

static void hle_lwmutex_create(ppu_context* ctx) {
    uint32_t maddr = (uint32_t)ctx->gpr[3];
    uint32_t aaddr = (uint32_t)ctx->gpr[4];
    sys_lwmutex_attribute_t attr;
    if (aaddr) {
        attr.protocol  = vm_read32(aaddr);
        attr.recursive = vm_read32(aaddr + 4);
        for (int i = 0; i < 8; i++) attr.name[i] = (char)vm_read8(aaddr + 8 + i);
    } else {
        attr.protocol  = SYS_SYNC_PRIORITY;
        attr.recursive = SYS_SYNC_RECURSIVE;
        std::memset(attr.name, 0, 8);
    }
    sys_lwmutex_t_hle m; std::memset(&m, 0, sizeof(m));
    s32 rc = sys_lwmutex_create(&m, &attr);
    lwmutex_write(maddr, &m);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void hle_lwmutex_lock(ppu_context* ctx) {
    uint32_t maddr = (uint32_t)ctx->gpr[3];
    /* Corrupted/garbage mutex pointer (e.g. uninitialised FILE* lock): just
     * succeed so callers like printf don't spin. */
    if (maddr < 0x10000 || maddr >= 0x10000000) { ctx->gpr[3] = 0; return; }
    sys_lwmutex_t_hle m; lwmutex_read(maddr, &m);
    s32 rc = sys_lwmutex_lock(&m, ctx->gpr[4]);
    if (rc == 0)  /* stamp owner so the userland fast-path sees it as ours */
        m.lock_var = ((uint64_t)HLE_MAIN_TID << 32) | (m.lock_var & 0xFFFFFFFFu);
    lwmutex_write(maddr, &m);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void hle_lwmutex_trylock(ppu_context* ctx) {
    uint32_t maddr = (uint32_t)ctx->gpr[3];
    if (maddr < 0x10000 || maddr >= 0x10000000) { ctx->gpr[3] = 0; return; }
    sys_lwmutex_t_hle m; lwmutex_read(maddr, &m);
    s32 rc = sys_lwmutex_trylock(&m);
    if (rc == 0)
        m.lock_var = ((uint64_t)HLE_MAIN_TID << 32) | (m.lock_var & 0xFFFFFFFFu);
    lwmutex_write(maddr, &m);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void hle_lwmutex_unlock(ppu_context* ctx) {
    uint32_t maddr = (uint32_t)ctx->gpr[3];
    if (maddr < 0x10000 || maddr >= 0x10000000) { ctx->gpr[3] = 0; return; }
    sys_lwmutex_t_hle m; lwmutex_read(maddr, &m);
    s32 rc = sys_lwmutex_unlock(&m);
    if (rc == 0 && m.recursive_count == 0)  /* mark free (no owner) */
        m.lock_var = (0xFFFFFFFFull << 32) | (m.lock_var & 0xFFFFFFFFu);
    lwmutex_write(maddr, &m);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void hle_lwmutex_destroy(ppu_context* ctx) {
    uint32_t maddr = (uint32_t)ctx->gpr[3];
    sys_lwmutex_t_hle m; lwmutex_read(maddr, &m);
    s32 rc = sys_lwmutex_destroy(&m);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

struct RFunc2 { uint32_t guest_addr; void (*host_func)(void*); const char* name; };
extern "C" const RFunc2 g_recompiled_funcs[];
extern "C" const size_t g_recompiled_func_count;
#if defined(_WIN32)
#include <windows.h>
#endif
static void dump_guest_backtrace(const char* tag) {
#if defined(_WIN32)
    void* fr[48]; unsigned short kk = RtlCaptureStackBackTrace(0, 48, fr, nullptr);
    fprintf(stderr, "[bt] %s:\n", tag);
    for (unsigned short i = 0; i < kk; i++) {
        uintptr_t pc = (uintptr_t)fr[i];
        const RFunc2* best = nullptr; uintptr_t bp = 0, np = (uintptr_t)-1;
        for (size_t j = 0; j < g_recompiled_func_count; j++) {
            uintptr_t hp = (uintptr_t)g_recompiled_funcs[j].host_func;
            if (hp <= pc && hp > bp) { bp = hp; best = &g_recompiled_funcs[j]; } }
        if (!best) continue;
        for (size_t j = 0; j < g_recompiled_func_count; j++) {
            uintptr_t hp = (uintptr_t)g_recompiled_funcs[j].host_func;
            if (hp > bp && hp < np) np = hp; }
        if (pc - bp < (np - bp))
            fprintf(stderr, "    %s (0x%08X) +0x%llX\n", best->name,
                    best->guest_addr, (unsigned long long)(pc - bp));
    }
    fflush(stderr);
#else
    (void)tag;
#endif
}

static void hle_thread_get_id(ppu_context* ctx) {
    uint32_t ptr = (uint32_t)ctx->gpr[3];
    if (ptr) vm_write64(ptr, HLE_MAIN_TID);
    ctx->gpr[3] = 0;
}

/* Write a NUL-terminated C string into guest memory (bounded). */
static void hle_write_cstr(uint32_t addr, const char* s, uint32_t cap = 0xFFFFFFFFu) {
    if (!addr) return;
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) vm_write8(addr + i, (uint8_t)s[i]);
    vm_write8(addr + i, 0);
}

/* ---- cellGame (content/boot path) -------------------------------------
 * These fill caller-provided output buffers. The stub used to return CELL_OK
 * without writing them, leaving uninitialised stack garbage that the game then
 * strcpy'd — a runaway copy over non-terminated memory. Values mirror what
 * rpcs3 returns for this HDD title. */
static const char* SIMPSONS_DIRNAME      = "NPUB30563";
static const char* SIMPSONS_CONTENT_PATH = "/dev_hdd0/game/NPUB30563";
static const char* SIMPSONS_USRDIR_PATH  = "/dev_hdd0/game/NPUB30563/USRDIR";

static void hle_cellGameBootCheck(ppu_context* ctx) {        /* (type*,attr*,size*,dirName*) */
    if (ctx->gpr[3]) vm_write32((uint32_t)ctx->gpr[3], 2);   /* CELL_GAME_GAMETYPE_HDD */
    if (ctx->gpr[4]) vm_write32((uint32_t)ctx->gpr[4], 0);   /* attributes */
    if (ctx->gpr[5]) {                                        /* CellGameContentSize */
        vm_write32((uint32_t)ctx->gpr[5] + 0, 0x100000);     /* hddFreeSizeKB ~1GB */
        vm_write32((uint32_t)ctx->gpr[5] + 4, 0);            /* sizeKB */
        vm_write32((uint32_t)ctx->gpr[5] + 8, 0);            /* sysSizeKB */
    }
    hle_write_cstr((uint32_t)ctx->gpr[6], SIMPSONS_DIRNAME);
    ctx->gpr[3] = 0;
}
static void hle_cellGameContentPermit(ppu_context* ctx) {    /* (contentInfoPath*,usrdirPath*) */
    hle_write_cstr((uint32_t)ctx->gpr[3], SIMPSONS_CONTENT_PATH);
    hle_write_cstr((uint32_t)ctx->gpr[4], SIMPSONS_USRDIR_PATH);
    ctx->gpr[3] = 0;
}
static void hle_cellGameGetParamString(ppu_context* ctx) {   /* (id,buf,bufsize) */
    const char* v = "The Simpsons Arcade Game";
    hle_write_cstr((uint32_t)ctx->gpr[4], v, (uint32_t)ctx->gpr[5]);
    ctx->gpr[3] = 0;
}
static void hle_cellGameGetParamInt(ppu_context* ctx) {      /* (id,value*) */
    if (ctx->gpr[4]) vm_write32((uint32_t)ctx->gpr[4], 0);
    ctx->gpr[3] = 0;
}

/* Route a sysPrxForUser thread import to the runtime's real LV2 syscall handler
 * (sys_ppu_thread.c). The runtime's create/exit read args from gpr[] (offset 0,
 * identical in both ppu_context layouts) and build their own thread context, so
 * passing our lifter ctx is safe. The userland import ABI (r3=tid*, r4=entry,
 * r5=arg, r6=prio, r7=stack, r8=flags, r9=name) matches the runtime syscall. */
struct lv2_syscall_table;
extern "C" lv2_syscall_table g_lv2_syscalls;
static void hle_route_syscall(uint32_t num, ppu_context* ctx) {
    typedef int64_t (*lv2_fn)(ppu_context*);
    lv2_fn h = ((lv2_fn*)&g_lv2_syscalls)[num];
    ctx->gpr[3] = h ? (uint64_t)h(ctx) : 0;
}

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

/* cellGcmSys bridges (src/gcm_bridge.cpp). */
extern "C" {
void simpsons_gcm_init_body(ppu_context*);
void simpsons_gcm_get_configuration(ppu_context*);
void simpsons_gcm_get_control_register(ppu_context*);
void simpsons_gcm_set_flip_handler(ppu_context*);
void simpsons_gcm_address_to_offset(ppu_context*);
void simpsons_gcm_map_main_memory(ppu_context*);
void simpsons_gcm_get_label_address(ppu_context*);
void simpsons_gcm_get_tiled_pitch_size(ppu_context*);
void simpsons_gcm_default_cmd_size(ppu_context*);
void simpsons_gcm_default_segment_size(ppu_context*);
void simpsons_vo_get_state(ppu_context*);
void simpsons_vo_get_resolution(ppu_context*);
void simpsons_vo_get_configuration(ppu_context*);
void simpsons_vo_get_device_info(ppu_context*);
void simpsons_vo_get_number_of_device(ppu_context*);
void simpsons_vo_get_resolution_availability(ppu_context*);
void simpsons_vo_configure(ppu_context*);
/* cellSpurs HLE (src/spurs_hle.cpp). */
void spurs_attribute_initialize(ppu_context*);
void spurs_initialize2(ppu_context*);
void spurs_create_jobchain(ppu_context*);
void spurs_run_jobchain(ppu_context*);
void spurs_join_jobchain(ppu_context*);
void spurs_jobchain_attr_a(ppu_context*);
void spurs_jobchain_attr_b(ppu_context*);
}

/* Single HLE dispatch entry. Returns via ctx->gpr[3]. */
extern "C" void simpsons_hle(uint32_t nid, ppu_context* ctx) {
    /* PPC64 ELFv1 ABI: a caller saves its TOC (r2) to sp+0x28 around an
     * inter-module call and restores it via `ld r2,0x28(r1)` afterwards. The
     * lifter omits the save before `bl <import>`, so we perform it here — the
     * import doesn't change sp or the module, so writing the current r2 makes
     * the caller's post-call restore load the correct TOC. (flОw discovery #1.) */
    vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);

    /* Locate metadata for diagnostics + routing. */
    const ImportEntry* e = nullptr;
    for (size_t i = 0; i < g_import_count; i++)
        if (g_imports[i].nid == nid) { e = &g_imports[i]; break; }

    const char* lib  = e ? e->lib  : "?";
    const char* name = (e && e->name[0]) ? e->name : "<nid>";

    /* Skip the high-frequency sync/thread chatter so genuinely new HLE calls
     * (graphics/audio/fs init) stay visible in the trace. */
    bool noisy = (nid == 0x1573DC3F || nid == 0x1BC200F4 || nid == 0x350D454E ||
                  nid == 0xAEB78725);
    static int logged = 0;
    if (!noisy && logged++ < 400)
        fprintf(stderr, "[hle] %s::%s (nid=0x%08X)\n", lib, name, nid);

    if (e && is_stubbed_lib(lib)) {
        ctx->gpr[3] = 0; /* CELL_OK */
        return;
    }

    /* sysPrxForUser sync primitives — real semantics (see bridges above). */
    switch (nid) {
        case 0x2F85C0EF: hle_lwmutex_create(ctx);  return;
        case 0x1573DC3F: hle_lwmutex_lock(ctx);    return;
        case 0xAEB78725: hle_lwmutex_trylock(ctx); return;
        case 0x1BC200F4: hle_lwmutex_unlock(ctx);  return;
        case 0xC3476D0C: hle_lwmutex_destroy(ctx); return;
        case 0x350D454E: hle_thread_get_id(ctx);   return;
        case 0x24A1EA07: hle_route_syscall(41, ctx); return;  /* sys_ppu_thread_create */
        case 0xAFF080A4: hle_route_syscall(42, ctx); return;  /* sys_ppu_thread_exit */
        case 0xE6F2C1E7: /* sys_process_exit — guest is aborting; show why */
        case 0xA2C7BA64: /* sys_prx_exitspawn_with_level */
            { static int once = 0; if (!once++) dump_guest_backtrace("guest abort/exit"); }
            ctx->gpr[3] = 0; return;
        /* cellGame — fill content/boot output buffers (see handlers above). */
        case 0xF52639EA: hle_cellGameBootCheck(ctx);      return;
        case 0x70ACEC67: hle_cellGameContentPermit(ctx);  return;
        case 0x3A5D726A: hle_cellGameGetParamString(ctx); return;
        case 0xB7A45CAF: hle_cellGameGetParamInt(ctx);    return;
        /* cellGcmSys — RSX graphics (see src/gcm_bridge.cpp). */
        case 0x15BAE46B: simpsons_gcm_init_body(ctx);            return;  /* _cellGcmInitBody */
        case 0xE315A0B2: simpsons_gcm_get_configuration(ctx);    return;  /* GetConfiguration */
        case 0xA547ADDE: simpsons_gcm_get_control_register(ctx); return;  /* GetControlRegister */
        case 0xA41EF7E8: simpsons_gcm_set_flip_handler(ctx);     return;  /* SetFlipHandler */
        case 0x21AC3697: simpsons_gcm_address_to_offset(ctx);    return;  /* AddressToOffset */
        case 0xA114EC67: simpsons_gcm_map_main_memory(ctx);      return;  /* MapMainMemory */
        case 0xF80196C1: simpsons_gcm_get_label_address(ctx);    return;  /* GetLabelAddress */
        case 0x055BD74D: simpsons_gcm_get_tiled_pitch_size(ctx); return;  /* GetTiledPitchSize */
        case 0x5E2EE0F0: simpsons_gcm_default_cmd_size(ctx);     return;  /* GetDefaultCommandWordSize */
        case 0x8CDF8C70: simpsons_gcm_default_segment_size(ctx); return;  /* GetDefaultSegmentWordSize */
        /* cellVideoOut — real resolution/display state (see src/gcm_bridge.cpp). */
        case 0x887572D5: simpsons_vo_get_state(ctx);                  return;  /* GetState */
        case 0xE558748D: simpsons_vo_get_resolution(ctx);             return;  /* GetResolution */
        case 0x15B0B0CD: simpsons_vo_get_configuration(ctx);          return;  /* GetConfiguration */
        case 0x1E930EEF: simpsons_vo_get_device_info(ctx);            return;  /* GetDeviceInfo */
        case 0x75BBB672: simpsons_vo_get_number_of_device(ctx);       return;  /* GetNumberOfDevice */
        case 0xA322DB75: simpsons_vo_get_resolution_availability(ctx);return;  /* GetResolutionAvailability */
        case 0x0BAE8772: simpsons_vo_configure(ctx);                  return;  /* Configure */
        /* cellSpurs JobChain HLE (see src/spurs_hle.cpp). */
        case 0x95180230: spurs_attribute_initialize(ctx); return;  /* _cellSpursAttributeInitialize */
        case 0x30AA96C4: spurs_initialize2(ctx);          return;  /* InitializeWithAttribute2 */
        case 0x303C19CD: spurs_create_jobchain(ctx);      return;  /* CreateJobChainWithAttribute */
        case 0xF31731BB: spurs_run_jobchain(ctx);         return;  /* RunJobChain */
        case 0xA7C066DE: spurs_join_jobchain(ctx);        return;  /* JoinJobChain */
        case 0x3548F483: spurs_jobchain_attr_a(ctx);      return;  /* jobchain attr (unnamed) */
        case 0x9FEF70C2: spurs_jobchain_attr_b(ctx);      return;  /* jobchain attr setname (unnamed) */
        default: break;
    }

    /* TODO: bridge core libraries (cellGcmSys, cellAudio, sys_io, sys_fs,
     * cellSysutil, cellSysmodule, cellGame, cellRtc, cellSpurs) to the
     * ps3recomp module handlers. For now return CELL_OK so init paths
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
