/*
 * The Simpsons Arcade Game — recomp entry point
 *
 * Boot sequence:
 *   1. point sys_fs at the game directory
 *   2. vm_init()                     — allocate the 256 MB guest address space
 *   3. elf_load_segments()           — map PT_LOAD segments from EBOOT.elf
 *   4. register the guest-callback hook
 *   5. dereference the entry OPD      — code addr + TOC
 *   6. build the initial ppu_context and dispatch into the recompiled CRT
 *
 * This is the boot scaffold. Graphics (cellGcmSys/RSX), audio (cellAudio),
 * input (cellPad) and the CRT-heap priming are wired up as the boot trace
 * demands them (mirrors the flОw bring-up).
 */
#include "config.h"
#include "elf_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>

/* Pull the C++-linkage headers (be_t<> templates etc.) in first, so the
 * extern "C" runtime includes below re-hit guarded no-ops. */
#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>
#include <ps3emu/guest_call.h>

extern "C" {
    #include "runtime/memory/vm.h"
    #include "runtime/ppu/ppu_context.h"
    #include "runtime/syscalls/lv2_syscall_table.h"
    #include "runtime/syscalls/sys_ppu_thread.h"
}

/* ---- runtime globals this project must define exactly once ---- */
extern "C" uint8_t* vm_base = nullptr;
namespace vm { uint8_t* g_base = nullptr; }
lv2_syscall_table g_lv2_syscalls;
extern "C" char g_sys_fs_root[512];

/* provided by other TUs. simpsons_enter builds the initial *lifter-layout*
 * ppu_context and dispatches — main never touches that struct, since the
 * runtime's ppu_context (used here for vm_init etc.) has different field
 * offsets past gpr[]. */
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" const size_t g_recompiled_func_count;
extern "C" void simpsons_enter(uint32_t code, uint32_t stack_top);
extern "C" void simpsons_guest_caller(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void simpsons_install_crash_handler(void);
extern "C" int  simpsons_spu_selftest(void);   /* src/spu_runner.cpp */
extern "C" void simpsons_cri_watchdog_start(void);  /* src/vm_bridge.cpp */
extern "C" void simpsons_spurs_kernel_run(uint32_t cellspurs_ea, uint32_t spu_num); /* spu_runner_spurs.cpp */
extern "C" void simpsons_spurs_dispatch_test(void);                                 /* spu_runner_spurs.cpp */

/* Lifter-side thread runner (indirect_dispatch.cpp). Builds a fresh lifter
 * ppu_context — main.cpp uses the runtime layout and can't. */
extern "C" uint64_t simpsons_run_thread(uint32_t func, uint32_t toc, uint32_t arg,
                                        uint32_t sp, uint32_t tid);

/* Thread-entry trampoline the runtime's sys_ppu_thread_create invokes on each
 * new host thread (via g_ppu_thread_entry_trampoline). It receives the runtime
 * ppu_context the runtime built; the userland sys_ppu_thread_create passes the
 * entry as an OPD (function descriptor), so resolve it to (func,toc) before
 * dispatching. Exit r3 is written back for ppu_host_thread_proc's join status. */
extern "C" void simpsons_thread_entry(ppu_context* c) {
    uint32_t entry = (uint32_t)c->cia;                 /* runtime-layout cia: the OPD addr */
    uint32_t func  = vm_read32(entry);
    uint32_t toc   = vm_read32(entry + 4);
    if (func == 0) { func = entry; toc = (uint32_t)c->gpr[2]; }  /* not an OPD — raw entry */
    uint32_t cri_ptr = vm_read32(toc - 0x3C1C);   /* expect 0x22C9AC for the game TOC 0x1913A8 */
    fprintf(stderr, "[thread-entry] host_tid=%lu guest_tid=%llu entry=0x%08X func=0x%08X toc=0x%08X *(toc-0x3C1C)=0x%08X\n",
            GetCurrentThreadId(), (unsigned long long)c->thread_id, entry, func, toc, cri_ptr);
    uint64_t r3 = simpsons_run_thread(func, toc, (uint32_t)c->gpr[3],
                                      (uint32_t)c->gpr[1], (uint32_t)c->thread_id);
    c->gpr[3] = r3;
}

int main(int argc, char** argv) {
    const char* game_dir = (argc > 1) ? argv[1] : SIMPSONS_GAME_DIR;

    simpsons_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);  /* unbuffered: keep logs on crash */
    printf("=== The Simpsons Arcade Game — recomp (%s) ===\n", SIMPSONS_TITLE_ID);
    printf("[init] game dir: %s\n", game_dir);

    /* 1. filesystem root (cellFs / sys_fs path translation) */
    snprintf(g_sys_fs_root, sizeof(g_sys_fs_root), "%s", game_dir);

    /* 2. virtual memory */
    if (vm_init() != 0) { fprintf(stderr, "ERROR: vm_init failed\n"); return 1; }
    vm::g_base = vm_base;

    /* 2.0 SPU runtime self-test (real-SPU bring-up: validates the lifted at3dec
     * decoder + SPU runtime link + initialize in the game process). */
    simpsons_spu_selftest();

    /* 2.0b CRI/SPURS sync watchdog — satisfy the SPU-completion sync points the
     * stubbed SPURS never advances, so the boot can proceed past the CRI walls
     * and we can see whether the render path engages. */
    simpsons_cri_watchdog_start();

    /* OBSERVATION (cont.29): run the recompiled SPURS kernel against a zeroed
     * marker CellSpurs instance to reveal which offsets it reads (= the header
     * layout to reverse). Marker EA chosen high + unused so DMA EAs are obvious. */
    if (getenv("SIMPSONS_SPURS_KTEST")) simpsons_spurs_dispatch_test();

    /* 2a. LV2 syscall table — populate it with the runtime's handlers, or every
     * `sc` falls through to ENOSYS (the table is otherwise zero-filled). This
     * brings up sys_tty_write (guest debug output), sys_memory_*, the thread
     * and lwmutex/lwcond sync primitives, etc. */
    lv2_syscall_table_init(&g_lv2_syscalls);
    lv2_register_all_syscalls(&g_lv2_syscalls);

    /* 2b. Real PPU threading: the runtime spawns host threads that call this
     * trampoline. Without it, sys_ppu_thread_create is a no-op and the main
     * thread spins forever waiting on workers (DRM/license/SPURS). */
    vm_stack_alloc_init(&g_vm_stack_alloc);   /* thread stacks @ 0xD0000000 */
    g_ppu_thread_entry_trampoline = simpsons_thread_entry;

    /* 3. load the decrypted EBOOT.elf segments */
    char elf_path[640];
    snprintf(elf_path, sizeof(elf_path), "%s/USRDIR/EBOOT.elf", game_dir);
    if (!elf_load_segments(elf_path)) {
        /* also accept a bare EBOOT.elf in the game dir */
        snprintf(elf_path, sizeof(elf_path), "%s/EBOOT.elf", game_dir);
        if (!elf_load_segments(elf_path)) {
            fprintf(stderr, "ERROR: could not load EBOOT.elf (decrypt EBOOT.BIN first)\n");
            return 1;
        }
    }
    printf("[init] %zu recompiled functions registered\n", g_recompiled_func_count);

    /* 4. guest-callback hook for HLE -> guest dispatch */
    g_ps3_guest_caller = simpsons_guest_caller;

    /* 5. resolve the entry OPD -> (code, toc). The 0x10230 stub loads its own
     * TOC from the OPD, so we only need the code address here. */
    uint32_t opd  = elf_entry_opd();             /* 0x00186900 */
    uint32_t code = vm_read32(opd);
    uint32_t toc  = vm_read32(opd + 4);
    printf("[init] entry OPD 0x%08X -> code 0x%08X toc 0x%08X\n", opd, code, toc);

    /* 6. enter the recompiled CRT (lifter-layout context built in the dispatch TU) */
    printf("[init] entering recompiled CRT at 0x%08X ...\n\n", code);
    simpsons_enter(code, SIMPSONS_STACK_TOP);

    printf("\n[exit] returned from guest entry\n");
    return 0;
}
