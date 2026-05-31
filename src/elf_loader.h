/*
 * The Simpsons Arcade Game — ELF segment loader
 *
 * Loads the PT_LOAD segments from the decrypted EBOOT.elf into the
 * ps3recomp virtual address space. The recompiled code references guest
 * globals / rodata via vm_read/vm_write at their original addresses, so
 * the data segments must be present before execution. (The code segment
 * is loaded too so OPDs and in-text constants resolve.)
 */
#ifndef SIMPSONS_ELF_LOADER_H
#define SIMPSONS_ELF_LOADER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load all PT_LOAD segments from `elf_path` into virtual memory.
 * Returns true on success. */
bool elf_load_segments(const char* elf_path);

/* Read the ELF e_entry value (an OPD address for PS3 binaries). */
uint32_t elf_entry_opd(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMPSONS_ELF_LOADER_H */
