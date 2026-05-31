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

static inline uint8_t* xlat(uint64_t addr) { return vm_base + (uint32_t)addr; }

extern "C" {

uint8_t  vm_read8 (uint64_t a) { return *xlat(a); }
uint16_t vm_read16(uint64_t a) { uint16_t r; memcpy(&r, xlat(a), 2); return bswap16(r); }
uint32_t vm_read32(uint64_t a) { uint32_t r; memcpy(&r, xlat(a), 4); return bswap32(r); }
uint64_t vm_read64(uint64_t a) { uint64_t r; memcpy(&r, xlat(a), 8); return bswap64(r); }

void vm_write8 (uint64_t a, uint8_t  v) { *xlat(a) = v; }
void vm_write16(uint64_t a, uint16_t v) { uint16_t r = bswap16(v); memcpy(xlat(a), &r, 2); }
void vm_write32(uint64_t a, uint32_t v) { uint32_t r = bswap32(v); memcpy(xlat(a), &r, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t r = bswap64(v); memcpy(xlat(a), &r, 8); }

} /* extern "C" */

/* ---- LV2 syscall dispatch ---------------------------------------------- */
#include "recomp/ppu_recomp.h"

/* The runtime's syscall table is an array of fn ptrs at offset 0 of this
 * symbol (see ps3recomp runtime/syscalls/lv2_syscall_table.h). */
struct lv2_syscall_table;
extern "C" lv2_syscall_table g_lv2_syscalls;
typedef int64_t (*lv2_syscall_fn)(void* ctx);

extern "C" void lv2_syscall(ppu_context* ctx) {
    uint32_t num = (uint32_t)ctx->gpr[11];
    if (num >= 1024) { ctx->gpr[3] = (uint64_t)(int64_t)-38 /*ENOSYS*/; return; }
    lv2_syscall_fn* handlers = (lv2_syscall_fn*)&g_lv2_syscalls;
    lv2_syscall_fn h = handlers[num];
    if (h) {
        ctx->gpr[3] = (uint64_t)h((void*)ctx);
    } else {
        static int warned = 0;
        if (warned++ < 64)
            fprintf(stderr, "[lv2] unimplemented syscall %u (0x%X)\n", num, num);
        ctx->gpr[3] = (uint64_t)(int64_t)-38;
    }
}
