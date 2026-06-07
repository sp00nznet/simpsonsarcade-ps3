/*
 * The Simpsons Arcade Game — VM bridge
 *
 * Implements the lifter's vm_read / vm_write memory API (host-endian values,
 * 64-bit guest addresses truncated to 32-bit) over ps3recomp's flat vm_base,
 * performing the big-endian byte swaps. Also bridges the sc instruction to
 * the runtime's LV2 syscall table.
 *
 * Deliberately free of game-specific snapshot/vtable workarounds — add those
 * here only if/when the Simpsons emulator needs them.
 */
#include <cstdint>
#include <cstring>
#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <stdlib.h>
static inline uint16_t bswap16(uint16_t v) { return _byteswap_ushort(v); }
static inline uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v);  }
static inline uint64_t bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

extern "C" uint8_t* vm_base;  /* defined in main.cpp, set by vm_init() */

/* Guest address space is the full 32-bit range (4 GB reserved by vm_init).
 * The PS3 maps memory well above the 256 MB main RAM (RSX, sys_memory /
 * sys_mmapper regions at 0x3000_0000+, 0x9000_0000+, stack at 0xD000_0000),
 * so we must translate any 32-bit guest address — uncommitted pages are
 * committed on demand by the crash handler's VEH. Every uint32 address is in
 * range, so this is effectively a no-op kept only as a defensive guard. */
static const uint64_t GUEST_SIZE = 0x100000000ull;

static bool oob(uint32_t a, int bytes, const char* op) {
    if ((uint64_t)a + bytes <= GUEST_SIZE) return false;
    static int n = 0;
    if (n++ < 64) fprintf(stderr, "[vm] OOB %s @0x%08X (%d) -> ignored\n", op, a, bytes);
    return true;
}

static inline uint8_t* xlat(uint64_t addr) { return vm_base + (uint32_t)addr; }

extern "C" {

uint8_t  vm_read8 (uint64_t a) { if (oob((uint32_t)a,1,"r8"))  return 0; return *xlat(a); }
uint16_t vm_read16(uint64_t a) { if (oob((uint32_t)a,2,"r16")) return 0; uint16_t r; memcpy(&r, xlat(a), 2); return bswap16(r); }
extern "C" unsigned long GetCurrentThreadId(void);
static void vb_backtrace(const char* tag);   /* defined later in this TU */
static inline void watch_rw(const char* op, uint32_t a, uint32_t v) {
    if (a == 0x22C9C0) {   /* the local-mem heap allocator object pointer */
        static int n = 0;
        if (n++ < 40) {
            fprintf(stderr, "[heap] tid=%lu %s allocator@0x22C9C0 = 0x%08X\n",
                    GetCurrentThreadId(), op, v);
            if (op[0] == 'W') vb_backtrace("allocator write");
        }
    }
}
uint32_t vm_read32(uint64_t a) { if (oob((uint32_t)a,4,"r32")) return 0; uint32_t r; memcpy(&r, xlat(a), 4); uint32_t rv = bswap32(r); watch_rw("R", (uint32_t)a, rv); return rv; }
uint64_t vm_read64(uint64_t a) { if (oob((uint32_t)a,8,"r64")) return 0; uint64_t r; memcpy(&r, xlat(a), 8); return bswap64(r); }

void vm_write8 (uint64_t a, uint8_t  v) { if (oob((uint32_t)a,1,"w8"))  return; *xlat(a) = v; }
void vm_write16(uint64_t a, uint16_t v) { if (oob((uint32_t)a,2,"w16")) return; uint16_t r = bswap16(v); memcpy(xlat(a), &r, 2); }
void vm_write32(uint64_t a, uint32_t v) { if (oob((uint32_t)a,4,"w32")) return; watch_rw("W", (uint32_t)a, v); uint32_t r = bswap32(v); memcpy(xlat(a), &r, 4); }
void vm_write64(uint64_t a, uint64_t v) { if (oob((uint32_t)a,8,"w64")) return; uint64_t r = bswap64(v); memcpy(xlat(a), &r, 8); }

/* ---- CRI/SPURS sync watchdog (cont.25) -------------------------------------
 * The game's CRI middleware blocks on SPU work that our stubbed SPURS never
 * performs. There are (at least) two sync points, hit non-deterministically by
 * the racy multi-threaded boot: (a) the SPURS-instance state halfword at
 * *(0x22A6A8)+8 == 0xC1 (busy), and (b) the producer/consumer counter pair
 * 0x22A6B8 (produced) / 0x22A6BC (consumed). A background thread satisfies both
 * continuously, regardless of which spin the boot lands in — a band-aid to find
 * out whether the render path engages once the CRI work is treated as done. */
extern "C" uint32_t g_simpsons_spurs;   /* CellSpurs handle (set in spurs_hle) */
/* Set once the CRI deadlock phase is active (watchdog sees the SPU-busy state).
 * Gates the sem-wait band-aid so we only skip the CRI completion sems, not the
 * legitimate early-boot sem waits. */
/* Externally linkable so the GCM bridge can trip it the moment the render path
 * goes live (cellGcmGetControlRegister) — that point provably precedes the fatal
 * CRI sem_wait(6), and a thread that has already blocked inside the runtime's
 * sem_wait can no longer be rescued, so the flag must be set *before* the wait. */
extern "C" volatile int g_cri_wedged = 0;

static DWORD WINAPI cri_watchdog_thread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    int log_a = 0, log_b = 0;
    for (;;) {
        /* (a) SPU-task busy states: CRI poll loops spin while a state halfword
         * == 0xC1 (busy). The exact object varies, so scan the SPURS instance +
         * CRI data regions for any 0x00C1xxxx word and clear the busy byte. */
        uint32_t spurs = g_simpsons_spurs;
        struct { uint32_t lo, hi; } ranges[] = {
            { spurs ? spurs - 0x200 : 0, spurs ? spurs + 0x1000 : 0 },
            { 0x22A400, 0x22A800 },
            /* CRI/GCM heap: the live SPU-task structs land here (observed task
             * @0xCCE280 with +8 == 0x00C10000 busy). The game produces a fresh
             * task per command, so scan the whole window and clear every busy
             * halfword the absent SPURS job would otherwise have cleared. */
            { 0xCC0000, 0xCD0000 },
        };
        for (auto& rg : ranges) {
            if (rg.lo < 0x10000 || rg.hi >= 0x10000000) continue;
            for (uint32_t a = rg.lo; a < rg.hi; a += 4) {
                uint32_t w = vm_read32(a);
                if ((w >> 16) == 0xC1) {
                    vm_write32(a, w & 0x0000FFFFu);
                    g_cri_wedged = 1;   /* CRI deadlock phase active */
                    if (log_a++ < 12) fprintf(stderr, "[watchdog] cleared 0xC1 busy @0x%08X (was 0x%08X)\n", a, w);
                }
            }
        }
        /* (b) CRI producer/consumer counter: consumed should catch produced. */
        uint32_t prod = vm_read32(0x22A6B8), cons = vm_read32(0x22A6BC);
        if (prod != cons && prod < 0x10000000) {
            vm_write32(0x22A6BC, prod);
            if (log_b++ < 5) fprintf(stderr, "[watchdog] converged CRI counter 0x22A6BC -> %u\n", prod);
        }
        Sleep(1);
    }
}
extern "C" void simpsons_cri_watchdog_start(void) {
    CreateThread(nullptr, 0, cri_watchdog_thread, nullptr, 0, nullptr);
    fprintf(stderr, "[watchdog] CRI/SPURS sync watchdog started\n");
}

} /* extern "C" */

/* ---- LV2 syscall dispatch ---------------------------------------------- */
#include "recomp/ppu_recomp.h"

/* The runtime's syscall table is an array of fn ptrs at offset 0 of this
 * symbol (see ps3recomp runtime/syscalls/lv2_syscall_table.h). */
struct lv2_syscall_table;
extern "C" lv2_syscall_table g_lv2_syscalls;
typedef int64_t (*lv2_syscall_fn)(void* ctx);

/* Resolve the calling guest function chain for a blocking syscall (sem_wait),
 * so we can see which guest code is parked on a never-posted semaphore. */
struct RFuncVB { uint32_t guest_addr; void (*host_func)(void*); const char* name; };
extern "C" const RFuncVB g_recompiled_funcs[];
extern "C" const size_t   g_recompiled_func_count;
static void vb_backtrace(const char* tag) {
#if defined(_WIN32)
    void* fr[40]; unsigned short n = RtlCaptureStackBackTrace(0, 40, fr, nullptr);
    fprintf(stderr, "[bt] %s:\n", tag);
    for (unsigned short i = 0; i < n; i++) {
        uintptr_t pc = (uintptr_t)fr[i], bp = 0, np = (uintptr_t)-1; const RFuncVB* best = nullptr;
        for (size_t k = 0; k < g_recompiled_func_count; k++) {
            uintptr_t hp = (uintptr_t)g_recompiled_funcs[k].host_func;
            if (hp <= pc && hp > bp) { bp = hp; best = &g_recompiled_funcs[k]; } }
        if (!best) continue;
        for (size_t k = 0; k < g_recompiled_func_count; k++) {
            uintptr_t hp = (uintptr_t)g_recompiled_funcs[k].host_func;
            if (hp > bp && hp < np) np = hp; }
        if (pc - bp < (np - bp) && pc - bp < 0x100000)
            fprintf(stderr, "    %s (0x%08X)+0x%llX\n", best->name, best->guest_addr,
                    (unsigned long long)(pc - bp));
    }
    fflush(stderr);
#endif
}

extern "C" void lv2_syscall(ppu_context* ctx) {
    uint32_t num = (uint32_t)ctx->gpr[11];
    if (num >= 1024) { ctx->gpr[3] = (uint64_t)(int64_t)-38 /*ENOSYS*/; return; }
    lv2_syscall_fn* handlers = (lv2_syscall_fn*)&g_lv2_syscalls;
    lv2_syscall_fn h = handlers[num];
    /* TEMP: capture every sys_tty_write(403) byte to a clean side channel so
     * we can read the full guest output uninterleaved with HLE logs. */
    if (num == 403) {
        uint32_t buf = (uint32_t)ctx->gpr[4], len = (uint32_t)ctx->gpr[5];
        static FILE* tty = nullptr;
        if (!tty) tty = fopen("ttyout.txt", "wb");
        if (tty) { for (uint32_t i = 0; i < len && i < 4096; i++) fputc(vm_read8(buf + i), tty); fflush(tty); }
    }
    /* Trace semaphore create/wait/post (90/92/94), capped — the band-aided boot
     * spins these hundreds of thousands of times, so an uncapped trace buries the
     * log. Keep a small sample for diagnosing the multi-thread handshake. */
    if (num == 90 || num == 92 || num == 94) {
        static int sem_logged = 0;
        if (sem_logged++ < 60) {
            const char* op = num == 90 ? "sem_create" : num == 92 ? "sem_WAIT" : "sem_POST";
            unsigned long tid = 0;
#if defined(_WIN32)
            tid = GetCurrentThreadId();
#endif
            fprintf(stderr, "[sem] tid=%lu %s sem=%u (r4=0x%llX)\n", tid, op,
                    (unsigned)ctx->gpr[3], (unsigned long long)ctx->gpr[4]);
        }
    }
    if (num == 141 && (uint32_t)ctx->gpr[3] == 0x12C) {
        /* HLE-SPURS drive: the CRI command/task queues spin in
         *   while ( *(r27)[produced] != *(r28)[consumed] ) usleep(300)
         * waiting for a SPURS job to consume/complete the item. We don't run
         * SPU, so the consumed counter never catches up and the queue never
         * posts its completion semaphore -> the main thread deadlocks.
         * When a given counter pair is observably wedged (same addresses +
         * values for several iterations, produced != consumed), converge it
         * (consumed = produced): the queue then drains, finalizes, and posts
         * its sem — i.e. we treat the absent SPURS job as instantly complete. */
        uint32_t a_addr = (uint32_t)ctx->gpr[27], b_addr = (uint32_t)ctx->gpr[28];
        bool sane = a_addr >= 0x10000 && a_addr < 0x20000000 &&
                    b_addr >= 0x10000 && b_addr < 0x20000000 && a_addr != b_addr;
        if (sane) {
            uint32_t va = vm_read32(a_addr), vb = vm_read32(b_addr);
            static uint32_t s_a=0,s_b=0,s_va=0,s_vb=0; static int s_cnt=0, s_forced=0;
            if (a_addr==s_a && b_addr==s_b && va==s_va && vb==s_vb) s_cnt++;
            else { s_a=a_addr; s_b=b_addr; s_va=va; s_vb=vb; s_cnt=0; }
            if (s_cnt >= 6 && va != vb && s_forced < 256) {
                vm_write32(b_addr, va);          /* consumed = produced */
                if (s_forced < 12)
                    fprintf(stderr, "[hle-spurs] wedged CRI queue @cons=0x%08X: %u -> %u (prod=0x%08X)\n",
                            b_addr, vb, va, a_addr);
                s_forced++; s_cnt = 0;
            }
        }
    }
    /* CRI deadlock band-aid (Phase 1, to reach the render path): once wedged, the
     * CRI completion sems (2, 6) are never posted (the SPU work is absent), so the
     * CRI thread blocks forever. Fake the wait as satisfied so it proceeds. */
    if (num == 92 && g_cri_wedged) {
        uint32_t s = (uint32_t)ctx->gpr[3];
        if (s == 1 || s == 2 || s == 6 || s == 7) {
            static int sk = 0;
            if (sk++ < 40) fprintf(stderr, "[cri-force] skip sem_wait(%u) [deadlock band-aid]\n", s);
            /* Yield so the faked-acquire busy-loops don't starve the coordinator
             * thread that would actually make progress past CRI init. */
            Sleep(0);
            ctx->gpr[3] = 0; return;
        }
    }
    static int sc_logged = 0;
    if (sc_logged++ < 200)
        fprintf(stderr, "[lv2] syscall %u (0x%X) r3=0x%llX r4=0x%llX r5=0x%llX %s\n",
                num, num, (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5], h ? "" : "(unimpl)");
    if (h) {
        ctx->gpr[3] = (uint64_t)h((void*)ctx);
    } else {
        static int warned = 0;
        if (warned++ < 64)
            fprintf(stderr, "[lv2] unimplemented syscall %u (0x%X)\n", num, num);
        ctx->gpr[3] = (uint64_t)(int64_t)-38;
    }
}
