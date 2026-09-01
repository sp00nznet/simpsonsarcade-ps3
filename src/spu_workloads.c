/* spu_workloads.c -- register the title's lifted SPURS job binaries.
 *
 * The Simpsons Arcade Game routes its rendering through CRI middleware running
 * as SPURS job2 jobs on the "crTaskRunJobList" chain. The shared runtime walks
 * the chain and stages each job the way Sony's jm2 does, then looks the binary
 * up in the workload registry by FNV-1a-64 fingerprint. Without a registration
 * every job logged "dispatch MISS" and did nothing, so the buffers the render
 * generator reads stayed zeroed and every draw was rejected in get_pso.
 *
 * These two binaries are NOT embedded SPU ELFs -- they are raw job images the
 * title builds in main memory, so extract_spu_images.py cannot find them. They
 * were captured with SPU_DUMP_MISS=spu_dump and lifted by tools/relift.sh;
 * the fingerprints below are the ones the dispatcher printed for them.
 */
#include "spu_workload.h"

extern void spu_begin_image(int image_id);

extern void crjob_spu_func_00000000(spu_context*);
extern void crjob_spu_recomp_register(void);
extern void crlist_spu_func_00000000(spu_context*);
extern void crlist_spu_recomp_register(void);

void simpsons_spu_register_all(void)
{
    /* crTaskRunJobList SPU runtime, guest EA 0x00168100, 41984 bytes. */
    spu_begin_image(1); crjob_spu_recomp_register();
    spu_workload_register_img(0x3DAC72B6B208C7C0ULL, crjob_spu_func_00000000,
                              1, "cri_crTaskRunJobList");

    /* The chain's second job, guest EA 0x00172500, 816 bytes. */
    spu_begin_image(2); crlist_spu_recomp_register();
    spu_workload_register_img(0x84CC39BF3EF23DA3ULL, crlist_spu_func_00000000,
                              2, "cri_joblist");
}

/* Registered at startup rather than from a hook in the shared boot harness, so
 * the harness stays title-agnostic (same trick build_spu_workloads.py emits). */
__attribute__((constructor)) static void simpsons_spu_register_all_ctor(void)
{
    simpsons_spu_register_all();
}
