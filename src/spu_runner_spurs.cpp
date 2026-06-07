/*
 * The Simpsons Arcade Game — SPURS kernel harness (LLE bring-up).
 *
 * Runs the recompiled firmware SPURS kernel (libsre spu_0000, prefix-lifted as
 * spK_*) on the validated SPU engine. The kernel is the SPU-side scheduler: it
 * DMAs the CellSpurs instance, polls the workload table via lock-line atomics,
 * loads a workload's policy module, and runs it. Per-context dispatch (image_id)
 * lets the kernel/policy/job coexist at overlapping LS addresses.
 *
 * Bring-up use: run the kernel with a CellSpurs EA and watch its MFC DMA — the
 * offsets it reads off the instance ARE the CellSpurs header layout we need to
 * reverse + build via an cellSpursInitialize HLE.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "runtime/spu/spu_context.h"   /* has its own extern "C" guards */
#include "spu/spurs_kernel_image.h"
#include "spu/spurs_policy_image.h"

extern "C" {
void spu_begin_image(int image_id);
void spu_register_function(uint32_t addr, void (*fn)(spu_context*));
void spK_recomp_register(void);
void spP_recomp_register(void);         /* policy module (loaded at LS 0xA00) */
void spP_func_00001148(spu_context*);   /* policy real entry (vaddr 0x848 -> LS 0x1148) */
void spK_func_00000818(spu_context*);   /* kernel _start (entry 0x818) */
extern uint8_t* vm_base;
extern int g_spu_dma_log;               /* MFC DMA trace budget (spu_runner.cpp) */
uint32_t vm_read32(uint64_t);
void vm_write32(uint64_t, uint32_t);
void vm_write8(uint64_t, uint8_t);
}

static spu_context g_spurs_kctx;   /* 256 KB LS — keep off the stack */
static bool s_kernel_registered = false;
/* policy EA/size the PPU-side init pre-stages into the kernel's LS (0x3FF80 =
 * policy EA doubleword, 0x3FFC0 = size) so the kernel's dispatch DMAs it to
 * LS 0xA00 and branches there. Set by the dispatch test. */
static uint32_t g_stage_pea = 0, g_stage_psz = 0;
static uint32_t g_stage_arg = 0;   /* policy launch arg -> LS[0x3FF80] bytes 8-11 */
/* EA the kernel hands the policy in gpr[2] (-> LS[0xAC0], the policy's DMA EA
 * base). Read by the patched kernel dispatch (spK_func_000007D8). */
extern "C" uint32_t g_spurs_policy_ea2 = 0;
static void spurs_put_be32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

/* Build a minimal firmware-valid CellSpurs instance (rpcs3 layout) with one
 * ready workload pointing at a policy module, so the LLE kernel dispatches it.
 * Offsets (rel. to instance base): wklReadyCount1[0]@+0x00, wklMaxContention[0]
 * @+0x50, wklState1[0]@+0x80, wklEnabled@+0xB0, wklInfo1[0]@+0xB00 (.addr u64 +0,
 * .arg u64 +8, .size u32 +0x10). */
extern "C" void simpsons_spurs_build_instance(uint32_t spurs_ea, uint32_t policy_ea,
                                              uint32_t policy_size, uint32_t wkl_arg) {
    for (uint32_t o = 0; o < 0x2000; o += 4) vm_write32(spurs_ea + o, 0);  /* zero it */
    /* Workload-mgmt arrays are 16-byte chunks (16 workloads x 1 byte), packed in
     * +0x00..+0x80 (this firmware), per the kernel's GETLLAR processing. Make
     * workload 0 ready (= byte 0 of each chunk). */
    vm_write8 (spurs_ea + 0x00, 1);   /* wklReadyCount1[0]       */
    vm_write8 (spurs_ea + 0x10, 1);   /* wklReadyCount2/idle[0]  */
    vm_write8 (spurs_ea + 0x40, 1);   /* wklMinContention[0]     */
    vm_write8 (spurs_ea + 0x50, 1);   /* wklMaxContention[0]     */
    vm_write8 (spurs_ea + 0x70, 2);   /* per-wkl state[0] = RUNNABLE */
    /* wklInfo (policy EA + arg). rpcs3 puts it @+0xB00; the kernel DMAs it after
     * selection, so we'll learn the real offset from that DMA. Populate both the
     * rpcs3 offset and a couple of candidates. */
    static const uint32_t wkl_bases[3] = { 0xB00u, 0x2D00u, 0x900u };
    for (int i = 0; i < 3; i++) {
        uint32_t base = wkl_bases[i];
        vm_write32(spurs_ea + base + 0x00, 0);
        vm_write32(spurs_ea + base + 0x04, policy_ea);
        vm_write32(spurs_ea + base + 0x08, 0);
        vm_write32(spurs_ea + base + 0x0C, wkl_arg);
        vm_write32(spurs_ea + base + 0x10, policy_size);
    }
    fprintf(stderr, "[spurs-k] built instance @0x%08X: wkl0 ready, policy@0x%08X size=0x%X arg=0x%08X\n",
            spurs_ea, policy_ea, policy_size, wkl_arg);
}

/* Dispatch test: build a clean instance + place the policy binary, run the kernel
 * against it, observe whether it reaches the dispatch path (DMAs the policy). */
extern "C" void simpsons_spurs_dispatch_test(void) {
    const uint32_t SPURS_EA = 0x0E000000, POLICY_EA = 0x0E010000;
    memcpy(vm_base + POLICY_EA, spP_policy_image, spP_policy_image_len);
    simpsons_spurs_build_instance(SPURS_EA, POLICY_EA, spP_policy_image_len, 0xCD0380);
    g_stage_pea = POLICY_EA;                 /* pre-stage policy for dispatch */
    g_stage_psz = spP_policy_image_len;
    g_stage_arg = 0xCD0380;                  /* policy gpr[4] = job-chain/work arg */
    g_spurs_policy_ea2 = SPURS_EA;           /* policy gpr[2] = instance EA (it mirrors the kernel) */
    void simpsons_spurs_kernel_run(uint32_t, uint32_t);
    simpsons_spurs_kernel_run(SPURS_EA, 0);
}

/* Run the SPURS kernel once against a CellSpurs instance EA. */
extern "C" void simpsons_spurs_kernel_run(uint32_t cellspurs_ea, uint32_t spu_num) {
    if (!s_kernel_registered) {
        spu_begin_image(1);          /* image 1 = the SPURS scheduler (kernel + policy) */
        spK_recomp_register();
        spP_recomp_register();       /* policy @LS 0xA00 — distinct range, same image */
        /* The kernel branches to the policy LOAD BASE (LS 0xA00); the policy's
         * real entry is at LS 0x1148 (vaddr 0x848). On HW the base is an entry
         * stub; alias it to the real entry so dispatch reaches the policy. */
        spu_register_function(0xA00, spP_func_00001148);
        spu_begin_image(0);
        s_kernel_registered = true;
        fprintf(stderr, "[spurs-k] kernel image registered (%u-byte binary @LS 0x%X)\n",
                spK_kernel_image_len, spK_kernel_image_base);
    }
    spu_context_init(&g_spurs_kctx, spu_num);
    g_spurs_kctx.image_id = 1;
    /* stage the kernel binary (code + its lqr-loaded data tables) into LS */
    memcpy(&g_spurs_kctx.ls[spK_kernel_image_base], spK_kernel_image, spK_kernel_image_len);
    /* SPU thread args. The kernel forms the CellSpurs DMA EA as gpr[16]+gpr[17]
     * (gpr[17] comes from LS 0x1C0 = gpr[2]); the 64-bit EA lives in words[0:1]
     * (EAL = ._u32[1]). So the instance EA goes in gpr[16]._u32[1]. Also set the
     * conventional arg regs gpr[3]/gpr[4] in case other paths read them. */
    /* The kernel stashes gpr[2] -> LS[0x1C0] and uses it as the instance/
     * lock-line EA: the header DMA = gpr[16]+LS[0x1C0], and the GETLLAR policy/
     * workload-table load (func_00000324) = LS[0x1C0]. So put the 64-bit EA
     * (doubleword in words[0:1]) in gpr[2] and zero gpr[16]. */
    g_spurs_kctx.gpr[2]._u32[0] = 0;             /* EA high 32 */
    g_spurs_kctx.gpr[2]._u32[1] = cellspurs_ea;  /* EA low 32  */
    g_spurs_kctx.gpr[16]._u32[0] = 0;
    g_spurs_kctx.gpr[16]._u32[1] = 0;
    g_spurs_kctx.gpr[3]._u32[0] = cellspurs_ea;
    g_spurs_kctx.gpr[4]._u32[0] = spu_num;
    /* Pre-stage the policy EA (LS 0x3FF80 = {0, ea} doubleword) + size (LS
     * 0x3FFC0) the kernel reads to DMA the policy to LS 0xA00 + dispatch. */
    if (g_stage_pea) {
        spurs_put_be32(&g_spurs_kctx.ls[0x3FF80 + 0], 0);
        spurs_put_be32(&g_spurs_kctx.ls[0x3FF80 + 4], g_stage_pea);
        spurs_put_be32(&g_spurs_kctx.ls[0x3FF80 + 8], g_stage_arg);   /* policy gpr[4] arg */
        spurs_put_be32(&g_spurs_kctx.ls[0x3FFC0 + 0], g_stage_psz);
        fprintf(stderr, "[spurs-k] pre-staged policy EA=0x%08X size=0x%X arg=0x%08X @LS 0x3FF80/0x3FFC0\n",
                g_stage_pea, g_stage_psz, g_stage_arg);
    }
    g_spurs_kctx.pc = 0x818;
    g_spu_dma_log = 60;
    fprintf(stderr, "[spurs-k] running kernel: cellspurs=0x%08X spu=%u\n", cellspurs_ea, spu_num);
    spK_func_00000818(&g_spurs_kctx);
    fprintf(stderr, "[spurs-k] kernel returned: pc=0x%05X status=%u DMAs=%d\n",
            g_spurs_kctx.pc, g_spurs_kctx.status, 60 - g_spu_dma_log);
}
