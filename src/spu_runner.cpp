/*
 * The Simpsons Arcade Game — SPU execution bridge (real-SPU plan, steps 5/6)
 *
 * Drives the validated ps3recomp SPU engine (runtime/spu/spu_channels.c) with
 * the game's CRI task-runner SPU job — the recompiled SPU binary that was found
 * embedded raw in the game's .text at 0x168100 (src/spu/cri_job.c). The CRI
 * deadlock (sem 2) is because this SPU job never runs under our stubbed
 * cellSpurs; running it lets it process the CRI task ring and advance the
 * consumer counter that crCommandQueue waits on.
 *
 * MFC DMA reaches guest RAM automatically via the shared `vm_base` symbol.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "runtime/spu/spu_context.h"   /* has its own extern "C" guards (+ pulls C++ ps3types) */

extern "C" {
void spu_recomp_register(void);            /* registers the lifted CRI-job functions */
void spu_func_00000000(spu_context*);      /* CRI job entry (wrapped at LS 0) */
extern uint8_t* vm_base;
uint32_t vm_read32(uint64_t);
}

/* The CRI task-runner SPU binary lives in the loaded game image. */
static const uint32_t CRI_JOB_EA   = 0x168100;   /* eaBinary (main-mem addr in .text) */
static const uint32_t CRI_JOB_SIZE = 0xA400;     /* up to the next embedded job @0x172500 */

static spu_context g_spu_ctx;   /* 256 KB LS — keep off the stack */
extern "C" int g_spu_dma_log = 60;   /* MFC DMA trace budget (spu_dma.h) */

extern "C" void simpsons_spu_init(void) {
    static bool done = false;
    if (done) return;
    done = true;
    spu_recomp_register();
    fprintf(stderr, "[spu] runtime initialized (CRI task-runner job registered)\n");
}

/* Run the CRI SPU job once. `arg` is the job's preferred-slot argument (the
 * SPURS workload arg the kernel would pass — typically the CRI context EA).
 * Loads the SPU binary from guest RAM into local store and runs from entry. */
extern "C" void simpsons_spu_run_cri_job(uint32_t arg) {
    simpsons_spu_init();
    spu_context_init(&g_spu_ctx, 0);
    if (CRI_JOB_SIZE <= SPU_LS_SIZE)
        memcpy(g_spu_ctx.ls, vm_base + CRI_JOB_EA, CRI_JOB_SIZE);
    g_spu_ctx.gpr[3]._u32[0] = arg;     /* r3 = job arg (preferred slot) */
    g_spu_ctx.pc = 0;
    g_spu_dma_log = 40;                  /* reset MFC trace budget per run */
    spu_func_00000000(&g_spu_ctx);
    int dmas = 40 - g_spu_dma_log;       /* DMAs the job issued this run */
    fprintf(stderr, "[spu] CRI job arg=0x%08X -> pc=0x%05X status=%u DMAs=%d\n",
            arg, g_spu_ctx.pc, g_spu_ctx.status, dmas);
}

/* Run the CRI job with the SPURS Job2 ABI: r4 = job descriptor (CellSpursJob256)
 * staged in LS, r3 = job context (CellSpursJobContext2) in LS, r1 = stack. The
 * job reads descriptor[+0xF8] user-data + context[+0] and gates to 0x7288 (exit)
 * if fields are zero. Empirical reversal: stage the real descriptor + run, watch
 * the exit pc to learn which fields the work path needs. */
extern "C" void simpsons_spu_run_cri_job2(uint32_t descriptor_ea) {
    simpsons_spu_init();
    spu_context_init(&g_spu_ctx, 0);
    memcpy(g_spu_ctx.ls, vm_base + CRI_JOB_EA, CRI_JOB_SIZE);   /* job binary @LS 0 */
    const uint32_t DESC_LS = 0x10000, CTX_LS = 0x11000;
    memcpy(&g_spu_ctx.ls[DESC_LS], vm_base + descriptor_ea, 256);  /* stage descriptor */
    memset(&g_spu_ctx.ls[CTX_LS], 0, 256);                          /* minimal context */
    g_spu_ctx.gpr[1]._u32[0] = 0x3FF00;     /* stack (frame grows down ~64KB) */
    g_spu_ctx.gpr[3]._u32[0] = CTX_LS;      /* r3 = job context (LS ptr, non-null, aligned) */
    g_spu_ctx.gpr[4]._u32[0] = DESC_LS;     /* r4 = job descriptor (LS ptr, aligned) */
    g_spu_ctx.pc = 0;
    g_spu_dma_log = 60;
    fprintf(stderr, "[spu] CRI job2: desc@0x%08X -> LS 0x%X, ctx@LS 0x%X\n", descriptor_ea, DESC_LS, CTX_LS);
    spu_func_00000000(&g_spu_ctx);
    fprintf(stderr, "[spu] CRI job2 returned: pc=0x%05X status=%u DMAs=%d (0x7288=no-work exit, 0x9040=work)\n",
            g_spu_ctx.pc, g_spu_ctx.status, 60 - g_spu_dma_log);
}

/* Boot-time bring-up validation (registers the job; does not run it). */
extern "C" int simpsons_spu_selftest(void) {
    simpsons_spu_init();
    spu_context_init(&g_spu_ctx, 0);
    fprintf(stderr, "[spu] selftest: LS=%uKB, CRI job @0x%08X (%u-byte SPU binary in guest .text)\n",
            (unsigned)(SPU_LS_SIZE / 1024), CRI_JOB_EA, CRI_JOB_SIZE);
    return 0;
}
