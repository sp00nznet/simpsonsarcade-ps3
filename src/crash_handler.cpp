/*
 * The Simpsons Arcade Game — crash diagnostics + demand paging
 *
 * A vectored exception handler with two jobs:
 *
 *  1. Demand paging. vm_init() reserves the full 4 GB guest space but only
 *     commits the main-memory window (0x10000..0x10010000) and the stack
 *     region. Guest code legitimately touches the null page (0x0..0x10000,
 *     e.g. the CRT walking a NULL argv) and other sparse regions. On a fault
 *     inside [vm_base, vm_base+4GB) we commit the faulting 64 KB page and
 *     resume — mirrors the flОw bring-up. This keeps the boot trace moving
 *     instead of dying on the first uncommitted access.
 *
 *  2. Diagnostics. For a fault we can't satisfy (commit failure, or a wild
 *     host pointer outside the guest space) we map the faulting host RIP back
 *     to the containing recompiled PPC function and dump the guest call chain.
 */
#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>

struct RecompiledFunc { uint32_t guest_addr; void (*host_func)(void*); const char* name; };
extern "C" const RecompiledFunc g_recompiled_funcs[];
extern "C" const size_t         g_recompiled_func_count;
extern "C" uint8_t*             vm_base;

static const uintptr_t VM_SIZE = 0x100000000ull; /* 4 GB reserved guest space */

static LONG WINAPI simpsons_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t rip   = (uintptr_t)ep->ContextRecord->Rip;
    const uintptr_t fault = ep->ExceptionRecord->NumberParameters >= 2
                          ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    const bool write = ep->ExceptionRecord->NumberParameters >= 1 &&
                       ep->ExceptionRecord->ExceptionInformation[0] == 1;
    const uintptr_t base = (uintptr_t)vm_base;

    /* ---- demand paging: commit the faulting guest page and resume ---- */
    if (code == EXCEPTION_ACCESS_VIOLATION && base &&
        fault >= base && fault < base + VM_SIZE) {
        uintptr_t page = fault & ~(uintptr_t)0xFFFF;          /* 64 KB granularity */
        SIZE_T    span = 0x10000;
        if (page + span > base + VM_SIZE) span = (base + VM_SIZE) - page;
        if (VirtualAlloc((void*)page, span, MEM_COMMIT, PAGE_READWRITE)) {
            static int paged = 0;
            if (paged++ < 32)
                fprintf(stderr, "[vm-demand] committed guest 0x%08X (fault @0x%08X, #%d)\n",
                        (uint32_t)(page - base), (uint32_t)(fault - base), paged + 1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* fall through to diagnostics if the commit itself failed */
    }

    fprintf(stderr, "\n[CRASH] %s  host_rip=0x%p\n",
            code == EXCEPTION_STACK_OVERFLOW ? "stack overflow"
              : (write ? "write access violation" : "read access violation"),
            (void*)rip);
    /* Classify the fault address: is it a guest deref (vm_base + offset)
     * that bypassed the vm_read/vm_write oob guard, or a wild host pointer? */
    fprintf(stderr, "[CRASH] fault host=0x%p  vm_base=0x%p\n", (void*)fault, (void*)base);
    if (base && fault >= base && fault < base + 0x100000000ull) {
        fprintf(stderr, "[CRASH] -> GUEST deref @0x%08X (vm_base + offset; bypassed oob guard)\n",
                (uint32_t)(fault - base));
    } else if (base && fault < base && base - fault < 0x10000) {
        fprintf(stderr, "[CRASH] -> NULL-ish guest deref (vm_base - 0x%llX)\n",
                (unsigned long long)(base - fault));
    } else {
        fprintf(stderr, "[CRASH] -> wild host pointer (not vm-relative)\n");
    }

    /* Resolve a host return-address to the nearest guest function. */
    auto resolve = [](uintptr_t pc, uintptr_t* off) -> const RecompiledFunc* {
        const RecompiledFunc* best = nullptr; uintptr_t bestp = 0;
        for (size_t i = 0; i < g_recompiled_func_count; i++) {
            uintptr_t hp = (uintptr_t)g_recompiled_funcs[i].host_func;
            if (hp <= pc && hp > bestp) { bestp = hp; best = &g_recompiled_funcs[i]; }
        }
        if (best && pc - bestp < 0x40000) { *off = pc - bestp; return best; }
        return nullptr;
    };

    /* Walk the host stack; print the guest call chain (most recent first). */
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    fprintf(stderr, "[CRASH] guest call chain:\n");
    int shown = 0;
    for (USHORT i = 0; i < n && shown < 16; i++) {
        uintptr_t off = 0;
        const RecompiledFunc* f = resolve((uintptr_t)frames[i], &off);
        if (f) {
            fprintf(stderr, "    %s (guest 0x%08X) +0x%llX\n",
                    f->name, f->guest_addr, (unsigned long long)off);
            shown++;
        }
    }
    if (!shown) fprintf(stderr, "    (no recompiled frames; fault in runtime/helper)\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;  /* log, then let it terminate */
}

/* Resolve a host pc to the containing guest function (tight bound via the next
 * function's host start). */
static const RecompiledFunc* resolve_guest(uintptr_t pc, uintptr_t* off) {
    const RecompiledFunc* best = nullptr; uintptr_t bp = 0, np = (uintptr_t)-1;
    for (size_t i = 0; i < g_recompiled_func_count; i++) {
        uintptr_t hp = (uintptr_t)g_recompiled_funcs[i].host_func;
        if (hp <= pc && hp > bp) { bp = hp; best = &g_recompiled_funcs[i]; }
    }
    if (!best) return nullptr;
    for (size_t i = 0; i < g_recompiled_func_count; i++) {
        uintptr_t hp = (uintptr_t)g_recompiled_funcs[i].host_func;
        if (hp > bp && hp < np) np = hp;
    }
    if (pc - bp >= (np - bp)) return nullptr;
    if (pc - bp > 0x100000) return nullptr;   /* >1MB past start: not really inside */
    *off = pc - bp; return best;
}

/* Watchdog: periodically suspend the main thread and report the guest function
 * at its RIP. Reveals pure-recompiled-code loops that make no HLE calls. */
static HANDLE g_main_thread = nullptr;
extern "C" uint64_t* g_main_gpr;

/* Print up to 48 printable bytes of the guest string at `addr`. */
static void dump_guest_str(const char* tag, uint32_t addr) {
    if (!vm_base || addr == 0) { fprintf(stderr, "      %s=0x%08X <null>\n", tag, addr); return; }
    char buf[65]; int i = 0;
    for (; i < 64; i++) { char ch = (char)vm_base[addr + i]; if (!ch) break;
        buf[i] = (ch >= 32 && ch < 127) ? ch : '.'; }
    buf[i] = 0;
    fprintf(stderr, "      %s=0x%08X \"%s\"%s\n", tag, addr, buf, i == 64 ? "...(no NUL in 64)" : "");
}

/* Describe where a suspended thread's RIP is: a recompiled guest function, or
 * the system module it's blocked in (ntdll/kernelbase => a kernel wait). */
static void describe_rip(uintptr_t rip, char* out, size_t cap) {
    uintptr_t off = 0;
    const RecompiledFunc* f = resolve_guest(rip, &off);
    if (f) { snprintf(out, cap, "%s (0x%08X)+0x%llX", f->name, f->guest_addr,
                      (unsigned long long)off); return; }
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)rip, &hm);
    char mod[MAX_PATH] = "?";
    if (hm) { GetModuleFileNameA(hm, mod, sizeof(mod));
        const char* t = strrchr(mod, '\\'); if (t) memmove(mod, t + 1, strlen(t)); }
    snprintf(out, cap, "[%s]+0x%llX", mod, (unsigned long long)(rip - (uintptr_t)hm));
}

/* Snapshot every PPU/host thread in this process and report where each is. */
static void dump_all_threads(int tag_ms) {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    fprintf(stderr, "[threads @%dms]\n", tag_ms);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        SuspendThread(h);
        CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
        char where[320] = "?";
        if (GetThreadContext(h, &c)) describe_rip((uintptr_t)c.Rip, where, sizeof(where));
        ResumeThread(h);
        CloseHandle(h);
        fprintf(stderr, "    tid %5lu: %s\n", (unsigned long)te.th32ThreadID, where);
    }
    CloseHandle(snap);
    fflush(stderr);
}

static DWORD WINAPI simpsons_watchdog(LPVOID) {
    for (int s = 0; s < 40; s++) {
        Sleep(500);
        /* Once everything has settled into its (dead)lock, snapshot all threads
         * a few times to see who is stuck where. */
        if (s == 8 || s == 16 || s == 30)
            dump_all_threads((s + 1) * 500);
    }
    (void)dump_guest_str; (void)g_main_gpr;
    return 0;
}

extern "C" void simpsons_install_crash_handler(void) {
    AddVectoredExceptionHandler(1, simpsons_veh);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(nullptr, 0, simpsons_watchdog, nullptr, 0, nullptr);
}
#else
extern "C" void simpsons_install_crash_handler(void) {}
#endif
