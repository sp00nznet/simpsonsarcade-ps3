/*
 * The Simpsons Arcade Game — indirect-call dispatch
 *
 * Owns the guest_addr -> host_func map (from g_recompiled_funcs[], generated
 * by tools/gen_runtime.py) used to resolve `bctrl` indirect calls and OPD
 * trampolines. Also registers the guest-callback hook the ps3recomp HLE
 * runtime fires for cellSysutil/GCM/saveData callbacks.
 */
#include "recomp/ppu_recomp.h"
#include <ps3emu/guest_call.h>
#include <cstdio>
#include <cstdint>
#include <cstddef>
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

/* Dispatch an indirect call: ctx->cia holds the guest target address. */
extern "C" void ps3_indirect_call(ppu_context* ctx) {
    uint32_t addr = (uint32_t)ctx->cia;
    auto fn = simpsons_resolve(addr);
    if (fn) {
        fn((void*)ctx);
    } else {
        static int warned = 0;
        if (warned++ < 64)
            fprintf(stderr, "[dispatch] unresolved guest call 0x%08X\n", addr);
    }
}
