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

extern "C" void hle_guest_malloc_reset(void) {
    if (vm_base) memset(vm_base + SIMPSONS_HEAP_BASE, 0,
                        SIMPSONS_HEAP_END - SIMPSONS_HEAP_BASE);
    g_heap_ptr = SIMPSONS_HEAP_BASE;
    g_alloc_count = 0;
}
