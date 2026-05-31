/*
 * The Simpsons Arcade Game — ELF segment loader (implementation)
 *
 * Big-endian PPC64 ELF. Copies each PT_LOAD's file bytes to vm_base+vaddr
 * and zero-fills the BSS tail (memsz > filesz).
 */
#include "elf_loader.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" uint8_t* vm_base;

namespace {

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
uint64_t be64(const uint8_t* p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

uint32_t g_entry_opd = 0;

constexpr uint32_t PT_LOAD = 1;

} // namespace

extern "C" uint32_t elf_entry_opd(void) { return g_entry_opd; }

extern "C" bool elf_load_segments(const char* elf_path) {
    FILE* f = fopen(elf_path, "rb");
    if (!f) {
        fprintf(stderr, "[elf] cannot open %s\n", elf_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "[elf] read failed\n");
        fclose(f);
        free(buf);
        return false;
    }
    fclose(f);

    if (memcmp(buf, "\x7F" "ELF", 4) != 0) {
        fprintf(stderr, "[elf] not an ELF (decrypt EBOOT.BIN -> EBOOT.elf first)\n");
        free(buf);
        return false;
    }

    g_entry_opd = (uint32_t)be64(buf + 0x18);
    uint64_t phoff = be64(buf + 0x20);
    uint16_t phentsize = be16(buf + 0x36);
    uint16_t phnum = be16(buf + 0x38);

    int loaded = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t* ph = buf + phoff + (uint64_t)i * phentsize;
        uint32_t p_type = be32(ph + 0x00);
        if (p_type != PT_LOAD) continue;
        uint64_t p_offset = be64(ph + 0x08);
        uint64_t p_vaddr  = be64(ph + 0x10);
        uint64_t p_filesz = be64(ph + 0x20);
        uint64_t p_memsz  = be64(ph + 0x28);
        if (p_memsz == 0) continue;

        memcpy(vm_base + (uint32_t)p_vaddr, buf + p_offset, (size_t)p_filesz);
        if (p_memsz > p_filesz)
            memset(vm_base + (uint32_t)p_vaddr + (uint32_t)p_filesz, 0,
                   (size_t)(p_memsz - p_filesz));

        fprintf(stderr, "[elf] PT_LOAD vaddr=0x%08X filesz=0x%llX memsz=0x%llX\n",
                (uint32_t)p_vaddr, (unsigned long long)p_filesz,
                (unsigned long long)p_memsz);
        loaded++;
    }

    free(buf);
    fprintf(stderr, "[elf] loaded %d segments, entry OPD = 0x%08X\n", loaded, g_entry_opd);
    return loaded > 0;
}
