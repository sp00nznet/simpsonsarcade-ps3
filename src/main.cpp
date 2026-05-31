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
}

/* ---- runtime globals this project must define exactly once ---- */
extern "C" uint8_t* vm_base = nullptr;
namespace vm { uint8_t* g_base = nullptr; }
lv2_syscall_table g_lv2_syscalls;
extern "C" char g_sys_fs_root[512];

/* provided by other TUs. ps3_indirect_call/vm_read32 take the lifter's
 * ppu_context, which is layout-compatible with the runtime's (gpr[] at
 * offset 0); extern "C" means no name mangling, so the pointer passes
 * straight through. */
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" const size_t g_recompiled_func_count;
extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" void simpsons_guest_caller(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);

int main(int argc, char** argv) {
    const char* game_dir = (argc > 1) ? argv[1] : SIMPSONS_GAME_DIR;

    printf("=== The Simpsons Arcade Game — recomp (%s) ===\n", SIMPSONS_TITLE_ID);
    printf("[init] game dir: %s\n", game_dir);

    /* 1. filesystem root (cellFs / sys_fs path translation) */
    snprintf(g_sys_fs_root, sizeof(g_sys_fs_root), "%s", game_dir);

    /* 2. virtual memory */
    if (vm_init() != 0) { fprintf(stderr, "ERROR: vm_init failed\n"); return 1; }
    vm::g_base = vm_base;

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

    /* 5. resolve the entry OPD -> (code, toc) */
    uint32_t opd  = elf_entry_opd();             /* 0x00186900 */
    uint32_t code = vm_read32(opd);
    uint32_t toc  = vm_read32(opd + 4);
    printf("[init] entry OPD 0x%08X -> code 0x%08X toc 0x%08X\n", opd, code, toc);

    /* 6. initial PPU context */
    ppu_context ctx;
    ppu_context_init(&ctx);
    ppu_set_stack(&ctx, 0x0F000000, SIMPSONS_STACK_SIZE);
    ctx.gpr[2] = toc;
    ctx.cia    = code;
    ctx.lr     = 0;   /* TODO: point at the sys_process_exit import stub */

    printf("[init] entering recompiled CRT at 0x%08X ...\n\n", code);
    ps3_indirect_call(&ctx);

    printf("\n[exit] returned from guest entry\n");
    return 0;
}
