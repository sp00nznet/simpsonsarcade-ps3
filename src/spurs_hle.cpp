/*
 * The Simpsons Arcade Game — cellSpurs HLE (real-SPU plan, step 6)
 *
 * The game's CRI middleware drives its SPU work through cellSpurs job chains.
 * With cellSpurs stubbed, the SPURS instance is never really set up, the game
 * never materializes its job command lists, and the CRI producer/consumer
 * counters never advance (the SPU side that would advance them is absent) —
 * hence the sem-2 deadlock.
 *
 * This module is the HLE that drives the validated SPU engine (src/spu_runner)
 * from the game's cellSpurs calls. Bring-up is incremental: first capture the
 * full job-chain activity (which calls, which command lists, which job
 * binaries) so we can see the real deadlocking job, then dispatch it on the SPU.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "recomp/ppu_recomp.h"   /* lifter-layout ppu_context */

extern "C" {
extern uint8_t* vm_base;
uint32_t vm_read32(uint64_t);
void     vm_write32(uint64_t, uint32_t);
extern volatile int g_cri_wedged;   /* vm_bridge.cpp: gates the CRI sem band-aid */
}

/* Tracked SPURS state. */
static uint32_t g_spurs = 0;             /* the CellSpurs handle (guest EA) */
extern "C" uint32_t g_simpsons_spurs = 0; /* mirror for the CRI watchdog (vm_bridge) */
static int      g_run_count = 0;

static void dump_cmdlist(const char* tag, uint32_t ea, int words) {
    if (ea < 0x10000 || ea >= 0x10000000) { fprintf(stderr, "[spurs] %s ea=0x%08X (oob)\n", tag, ea); return; }
    fprintf(stderr, "[spurs] %s @0x%08X:", tag, ea);
    int nonzero = 0;
    for (int i = 0; i < words; i++) { uint32_t v = vm_read32(ea + i*4); if (v) nonzero++; }
    fprintf(stderr, " (%d/%d nonzero)\n", nonzero, words);
    for (int i = 0; i < words && nonzero; i++) {
        uint32_t v = vm_read32(ea + i*4);
        if (v) fprintf(stderr, "    +0x%02X = 0x%08X\n", i*4, v);
    }
}

/* _cellSpursAttributeInitialize(attr, nSpus, spuPrio, ppuPrio, exitIfNoWork) */
extern "C" void spurs_attribute_initialize(ppu_context* ctx) {
    fprintf(stderr, "[spurs] AttributeInitialize attr=0x%08llX nSpus=%llu\n",
            (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4]);
    ctx->gpr[3] = 0;
}

/* cellSpursInitializeWithAttribute2(spurs, attr) — set up the SPURS instance. */
extern "C" void spurs_initialize2(ppu_context* ctx) {
    g_spurs = (uint32_t)ctx->gpr[3];
    g_simpsons_spurs = g_spurs;
    fprintf(stderr, "[spurs] InitializeWithAttribute2 spurs=0x%08X attr=0x%08llX\n",
            g_spurs, (unsigned long long)ctx->gpr[4]);
    ctx->gpr[3] = 0;
}

/* cellSpursCreateJobChainWithAttribute(spurs, jobChain, attr) */
extern "C" void spurs_create_jobchain(ppu_context* ctx) {
    uint32_t jc = (uint32_t)ctx->gpr[4], attr = (uint32_t)ctx->gpr[5];
    static int nc = 0; int k = nc++;
    fprintf(stderr, "[spurs] CreateJobChain #%d spurs=0x%08llX jobChain=0x%08X attr=0x%08X\n",
            k, (unsigned long long)ctx->gpr[3], jc, attr);
    /* The attribute carries the job-chain command list EA + descriptor sizing.
     * Dump it (first few calls) so we can find the real command list to walk. */
    if (k < 4) {
        dump_cmdlist("  jobChain struct", jc, 24);
        /* attr can live high in guest RAM (stack/TLS); raw-dump it regardless. */
        fprintf(stderr, "[spurs]   attr @0x%08X:\n", attr);
        for (int i = 0; i < 24; i++) {
            uint32_t v = vm_read32(attr + i*4);
            if (v) fprintf(stderr, "    +0x%02X = 0x%08X\n", i*4, v);
        }
    }
    ctx->gpr[3] = 0;
}

/* cellSpursRunJobChain(jobChain) — THE dispatch point. Capture every call's
 * command list so we can find the deadlocking job + its binary. */
extern "C" void simpsons_spu_run_cri_job(uint32_t arg);  /* spu_runner.cpp */
extern "C" void simpsons_spurs_kernel_run(uint32_t cellspurs_ea, uint32_t spu_num); /* spu_runner_spurs.cpp */
extern "C" void simpsons_spu_run_cri_job2(uint32_t descriptor_ea);                  /* spu_runner.cpp */

extern "C" void spurs_run_jobchain(ppu_context* ctx) {
    uint32_t jc = (uint32_t)ctx->gpr[3];
    int n = ++g_run_count;
    /* The CRI job chain ('crTaskRunJobList') is CRI middleware; the game blocks on
     * its completion sems (2/6/7) which nothing posts (the SPU side is absent).
     * Trip the band-aid HERE — the earliest reliable point, before the multi-thread
     * completion-sem race — so the boot proceeds to its render path regardless of
     * GCM-init timing. (Tests whether the CRI tasks are on the render critical path.) */
    g_cri_wedged = 1;
    /* Track distinct jobChain handles + dump each one's structure (capped): is the
     * game cycling ONE chain (stuck) or many (main loop)? And where's its cmd list? */
    { static uint32_t seen[8]; static int ns = 0; bool found = false;
      for (int i = 0; i < ns; i++) if (seen[i] == jc) found = true;
      if (!found && ns < 8) { seen[ns++] = jc;
          fprintf(stderr, "[spurs] RunJobChain #%d NEW jobChain=0x%08X (distinct=%d)\n", n, jc, ns);
          dump_cmdlist("  jc struct", jc, 32); }
      else if (n <= 12)
          fprintf(stderr, "[spurs] RunJobChain #%d jobChain=0x%08X (seen)\n", n, jc);
    }
    /* Run the CRI task-runner SPU job (recompiled). Candidate arg = the CRI
     * context base near the task counter (0x22A6A0); the DMA trace reveals what
     * it actually reads so we can correct the arg/job-ABI. */
    if (n == 1) {
        /* The game-built job descriptors (CellSpursJobHeader: eaBinary @+4 +
         * input/output DMA lists + args) — the SPURS job ABI the kernel reads. */
        dump_cmdlist("jobdesc @0xCD0380", 0xCD0380, 32);
        dump_cmdlist("jobdesc @0x22C880", 0x22C880, 32);
        /* eaHList = the job's I/O DMA list (jobdesc+0x34). Dump it + follow each
         * plausible guest-pointer one level to find the task buffers/ring. */
        uint32_t hlist = vm_read32(0xCD0380 + 0x34);   /* = 0xCD0480 */
        dump_cmdlist("eaHList @CD0480", hlist, 48);
        for (int i = 0; i < 48; i++) {
            uint32_t p = vm_read32(hlist + i*4);
            if (p >= 0x10000 && p < 0x10000000)
                fprintf(stderr, "    HList[+0x%02X]=0x%08X -> [%08X %08X %08X %08X]\n", i*4, p,
                        vm_read32(p), vm_read32(p+4), vm_read32(p+8), vm_read32(p+0xC));
        }
    }
    if (n == 1 && g_spurs) {
        /* Dump the CellSpurs instance the game left (firmware init is stubbed) to
         * reverse what the SPURS kernel needs. Firmware layout (rpcs3): +0x00
         * wklReadyCount1[16], +0x10 wklReadyCount2/idle[16], then wkl enable/
         * status masks + the per-workload info array (policy module EA, arg). */
        fprintf(stderr, "[spurs] === CellSpurs instance @0x%08X (firmware state) ===\n", g_spurs);
        dump_cmdlist("spurs+0x000", g_spurs + 0x000, 16);   /* ready counts (the 32B header) */
        dump_cmdlist("spurs+0x080", g_spurs + 0x080, 16);
        dump_cmdlist("spurs+0x100", g_spurs + 0x100, 16);
        dump_cmdlist("spurs+0x900", g_spurs + 0x900, 32);   /* wkl info array region */
        /* Job2-ABI reversal: dump the full job descriptor + run the CRI job with
         * the Job2 ABI (r4=descriptor LS, r3=context LS). Watch the exit pc. */
        dump_cmdlist("jobdesc full @0xCD0380", 0xCD0380, 64);
        simpsons_spu_run_cri_job2(0xCD0380);
    }
    ctx->gpr[3] = 0;
}

/* cellSpursJoinJobChain(jobChain) */
extern "C" void spurs_join_jobchain(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* JobChain attribute helpers (unnamed NIDs 0x3548F483, 0x9FEF70C2). */
extern "C" void spurs_jobchain_attr_a(ppu_context* ctx) {
    fprintf(stderr, "[spurs] jcAttrA(0x3548F483) r3=%llu r4=0x%08llX r5=0x%08llX r6=0x%08llX r7=0x%08llX\n",
            (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
            (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6],
            (unsigned long long)ctx->gpr[7]);
    /* Hunt for the job binary: the SPURS job command list / descriptors point
     * to a CellSpursJobHeader whose eaBinary is the SPU code. Dump candidates. */
    dump_cmdlist("r4(cmdlist)", (uint32_t)ctx->gpr[4], 24);
    dump_cmdlist("r6(descriptors)", (uint32_t)ctx->gpr[6], 24);
    /* follow each guest-pointer in r6's region one level (looking for eaBinary). */
    uint32_t r6 = (uint32_t)ctx->gpr[6];
    if (r6 >= 0x10000 && r6 < 0x10000000) {
        for (int i = 0; i < 24; i++) {
            uint32_t p = vm_read32(r6 + i*4);
            if (p > 0x10000 && p < 0x10000000) {
                uint32_t w0 = vm_read32(p), w1 = vm_read32(p+4);
                fprintf(stderr, "    *r6[+0x%02X]=0x%08X -> [0x%08X 0x%08X]\n", i*4, p, w0, w1);
            }
        }
    }
    ctx->gpr[3] = 0;
}
extern "C" void spurs_jobchain_attr_b(ppu_context* ctx) {
    uint32_t name = (uint32_t)ctx->gpr[4];
    char buf[32] = {0};
    for (int i = 0; i < 31 && name; i++) { char c = (char)vm_base[name + i]; if (!c) break; buf[i] = (c>=32&&c<127)?c:'.'; }
    fprintf(stderr, "[spurs] jcAttrB(0x9FEF70C2) attr=0x%08llX name='%s'\n",
            (unsigned long long)ctx->gpr[3], buf);
    ctx->gpr[3] = 0;
}
