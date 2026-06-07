/*
 * The Simpsons Arcade Game — indirect-call dispatch
 *
 * Owns the guest_addr -> host_func map (from g_recompiled_funcs[], generated
 * by tools/gen_runtime.py) used to resolve `bctrl` indirect calls and OPD
 * trampolines. Also registers the guest-callback hook the ps3recomp HLE
 * runtime fires for cellSysutil/GCM/saveData callbacks.
 */
#include "recomp/ppu_recomp.h"
#include "config.h"
#include <ps3emu/guest_call.h>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <unordered_map>

struct RecompiledFunc { uint32_t guest_addr; void (*host_func)(void* ctx); const char* name; };
extern "C" const RecompiledFunc g_recompiled_funcs[];
extern "C" const size_t         g_recompiled_func_count;

/* Defined here (declared extern in guest_call.h); set in main. */
extern "C" ps3_guest_caller_fn g_ps3_guest_caller = nullptr;

/* Thread-local trampoline pointer for split-function cross-fragment
 * fallthrough. The lifter declares this extern and drains it via
 * DRAIN_TRAMPOLINE after every call; we own the definition. */
#if defined(_MSC_VER)
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;
#else
extern "C" thread_local void (*g_trampoline_fn)(void*) = nullptr;
#endif

static std::unordered_map<uint32_t, void(*)(void*)>& table() {
    static std::unordered_map<uint32_t, void(*)(void*)> m;
    if (m.empty()) {
        m.reserve(g_recompiled_func_count * 2);
        for (size_t i = 0; i < g_recompiled_func_count; i++)
            m[g_recompiled_funcs[i].guest_addr] = g_recompiled_funcs[i].host_func;
    }
    return m;
}

/* Resolve a guest code address to its host function (nullptr if unlifted). */
extern "C" void (*simpsons_resolve(uint32_t addr))(void*) {
    auto& m = table();
    auto it = m.find(addr);
    return it == m.end() ? nullptr : it->second;
}

/* Sentinel LR for the initial frame: a blr/bctr to it means "returned to OS". */
static const uint32_t SIMPSONS_EXIT_LR = 0xFFFFFFF0u;

/* Dispatch an indirect call (bctr/bctrl). The lifter models these by loading
 * the target guest address into CTR (via mtctr, or from an OPD's func field)
 * and emitting `ps3_indirect_call(ctx)` — so the target is ctx->ctr, NOT
 * ctx->cia (cia is not maintained by the lifted code). */
extern "C" void ps3_indirect_call(ppu_context* ctx) {
    uint32_t addr = (uint32_t)ctx->ctr;
    if (addr == 0 || addr == SIMPSONS_EXIT_LR) {
        fprintf(stderr, "[dispatch] guest returned to OS (ctr=0x%08X)\n", addr);
        return;
    }
    /* Recursion-depth guard: some render-init code dispatches recursively
     * through jump tables (e.g. func_0014B7B0). A malformed/cyclic structure
     * makes that recurse without bound and blow the host stack. Cap the depth:
     * log the runaway target once, then skip the call so the recursion unwinds
     * and the boot can continue (surfacing the next real blocker). */
    static thread_local int s_depth = 0;
    if (addr == 0x0014EBB0) {
        static int sh = 0;
        if (sh++ < 3) {
            extern uint16_t vm_read16(uint64_t);
            uint32_t toc = (uint32_t)ctx->gpr[2];
            uint32_t h   = vm_read32(toc - 0x3B98);          /* allocator handle ptr */
            uint32_t al  = h ? vm_read32(h) : 0;             /* allocator object */
            uint16_t cnt = al ? vm_read16(al + 0xA) : 0;     /* free-list count */
            uint32_t desc = (uint32_t)ctx->gpr[31];          /* surface descriptor */
            uint32_t g90 = vm_read32(toc - 0x3B90);
            uint32_t g9C = vm_read32(toc - 0x3B9C);
            fprintf(stderr, "[rec] EBB0 depth=%d desc=0x%08X (buf@+0xC=0x%08X) | "
                    "obj=0x%08X freecount=%u | localBase=0x%08X localSize=0x%08X\n",
                    s_depth, desc, vm_read32(desc + 0xC), al, cnt,
                    g9C ? vm_read32(g9C) : 0, g90 ? vm_read32(g90 + 0x10) : 0);
        }
    }
    if (s_depth > 1500) {
        static int warned = 0;
        if (warned++ < 8)
            fprintf(stderr, "[dispatch] recursion cap hit @0x%08X depth=%d "
                    "(r28=0x%08X r29=0x%08X r3=0x%08X) — skipping\n", addr, s_depth,
                    (uint32_t)ctx->gpr[28], (uint32_t)ctx->gpr[29], (uint32_t)ctx->gpr[3]);
        return;
    }
    auto fn = simpsons_resolve(addr);
    if (fn) {
        s_depth++;
        fn((void*)ctx);
        s_depth--;
        /* Drain the tail-call trampoline chain. Lifted functions model a tail
         * call (`b target`) by setting g_trampoline_fn and returning, expecting
         * the caller's DRAIN_TRAMPOLINE to continue. An indirect call / thread
         * entry IS such a caller: if the dispatched function ends in a tail call
         * (e.g. func_00131710 -> func_00131770, the crTaskQueue work loop), the
         * continuation is lost unless we drain it here too. */
        while (g_trampoline_fn) {
            void (*tf)(void*) = g_trampoline_fn;
            g_trampoline_fn = nullptr;
            tf((void*)ctx);
        }
    } else {
        static int warned = 0;
        if (warned++ < 64)
            fprintf(stderr, "[dispatch] unresolved guest call 0x%08X\n", addr);
    }
}

extern "C" uint8_t* vm_base;
extern "C" uint64_t* g_main_gpr = nullptr;   /* live gpr[] of the main guest thread (watchdog) */

/* Build a fresh lifter-layout context, set up the main-thread TLS, and enter
 * the guest at `code`. Owned here (not main.cpp) because this TU has the
 * lifter's ppu_context; main.cpp uses the runtime's, whose field offsets
 * differ past gpr[]. */
extern "C" void simpsons_enter(uint32_t code, uint32_t stack_top) {
    /* Lay out the main-thread TLS image at TP-0x7000: copy the PT_TLS
     * initialized bytes, zero the rest (BSS tail). The recompiled CRT then
     * accesses thread-locals relative to r13. */
    std::memcpy(vm_base + SIMPSONS_TLS_IMG,
                vm_base + SIMPSONS_TLS_TEMPLATE, SIMPSONS_TLS_FILESZ);
    std::memset(vm_base + SIMPSONS_TLS_IMG + SIMPSONS_TLS_FILESZ, 0,
                SIMPSONS_TLS_MEMSZ - SIMPSONS_TLS_FILESZ);

    ppu_context ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    g_main_gpr = ctx.gpr;           /* publish for the watchdog (file-scope global) */
    ctx.gpr[1]  = stack_top;        /* SP */
    ctx.gpr[13] = SIMPSONS_TLS_TP;  /* thread pointer */
    ctx.lr      = SIMPSONS_EXIT_LR; /* return-to-OS sentinel */
    ctx.ctr     = code;
    ctx.cia     = code;
    ps3_indirect_call(&ctx);
}

/* Run a recompiled PPU thread on the calling host thread. Called from the
 * runtime's thread-entry trampoline (simpsons_thread_entry in main.cpp) after
 * it has resolved the entry OPD. We build a fresh *lifter-layout* context here
 * (main.cpp can't — it uses the runtime layout) so the dispatched code reads
 * cia/lr/ctr/etc. at the right offsets. Returns the thread's exit r3. */
extern "C" uint64_t simpsons_run_thread(uint32_t func, uint32_t toc, uint32_t arg,
                                        uint32_t sp, uint32_t tid) {
    /* Per-thread TLS image (TP = img + 0x7000), copied from the PT_TLS template.
     * Lives in a dedicated demand-paged region so threads don't share errno etc.
     * with the main thread or each other. */
    uint32_t tls_img = 0x30000000u + tid * 0x20000u;
    std::memcpy(vm_base + tls_img,
                vm_base + SIMPSONS_TLS_TEMPLATE, SIMPSONS_TLS_FILESZ);
    std::memset(vm_base + tls_img + SIMPSONS_TLS_FILESZ, 0,
                SIMPSONS_TLS_MEMSZ - SIMPSONS_TLS_FILESZ);

    ppu_context ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[1]  = sp;
    ctx.gpr[2]  = toc;
    ctx.gpr[3]  = arg;
    ctx.gpr[13] = tls_img + 0x7000;
    ctx.lr      = SIMPSONS_EXIT_LR;
    ctx.ctr     = func;
    ctx.cia     = func;
    ps3_indirect_call(&ctx);
    return ctx.gpr[3];
}
