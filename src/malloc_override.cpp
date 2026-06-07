/*
 * The Simpsons Arcade Game — guest malloc HLE override
 *
 * The PS3 CRT's Dinkumware allocator needs OS heap init the recomp doesn't
 * reproduce. We replace it with a bump allocator in committed guest VM.
 * Wire this in once we identify the emulator's malloc/free entry points
 * (patch those func_<addr> in ppu_recomp.cpp to call hle_guest_malloc).
 */
#include "recomp/ppu_recomp.h"
#include "config.h"
#include <cstdio>
#include <cstring>

extern "C" uint8_t* vm_base;

static uint32_t g_heap_ptr = SIMPSONS_HEAP_BASE;
static uint32_t g_alloc_count = 0;

extern "C" void hle_guest_malloc(ppu_context* ctx) {
    uint32_t size = (uint32_t)ctx->gpr[3];
    size = (size + 15) & ~15u;
    if (size == 0) size = 16;
    if (g_heap_ptr + size > SIMPSONS_HEAP_END) {
        fprintf(stderr, "[malloc] OOM requesting %u bytes\n", size);
        ctx->gpr[3] = 0;
        return;
    }
    uint32_t ptr = g_heap_ptr;
    g_heap_ptr += size;
    g_alloc_count++;
    memset(vm_base + ptr, 0, size);
    ctx->gpr[3] = ptr;
}

extern "C" void hle_guest_free(ppu_context* ctx) { (void)ctx; }

/* realloc(ptr=r3, size=r4): allocate fresh and copy the old contents. We don't
 * track block sizes (bump allocator), so copy a bounded amount — enough for the
 * init-path reallocs that grow small buffers. */
extern "C" void hle_guest_realloc(ppu_context* ctx) {
    uint32_t old_ptr = (uint32_t)ctx->gpr[3];
    uint32_t size    = (uint32_t)ctx->gpr[4];
    if (size == 0) { ctx->gpr[3] = 0; return; }       /* realloc(p,0) == free */
    ctx->gpr[3] = size;
    hle_guest_malloc(ctx);                            /* sets gpr[3] = new ptr */
    uint32_t new_ptr = (uint32_t)ctx->gpr[3];
    if (new_ptr && old_ptr && vm_base) {
        uint32_t cp = size < 0x10000 ? size : 0x10000;
        memcpy(vm_base + new_ptr, vm_base + old_ptr, cp);
    }
}

/* calloc(n=r3, size=r4): n*size bytes, zeroed (hle_guest_malloc already zeroes). */
extern "C" void hle_guest_calloc(ppu_context* ctx) {
    uint64_t n = (uint32_t)ctx->gpr[3], s = (uint32_t)ctx->gpr[4];
    ctx->gpr[3] = (uint32_t)(n * s);
    hle_guest_malloc(ctx);
}

extern "C" void hle_guest_malloc_reset(void) {
    if (vm_base) memset(vm_base + SIMPSONS_HEAP_BASE, 0,
                        SIMPSONS_HEAP_END - SIMPSONS_HEAP_BASE);
    g_heap_ptr = SIMPSONS_HEAP_BASE;
    g_alloc_count = 0;
}
