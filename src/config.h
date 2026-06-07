/*
 * The Simpsons Arcade Game — Recomp build configuration
 */
#ifndef SIMPSONS_CONFIG_H
#define SIMPSONS_CONFIG_H

/* Game metadata */
#ifndef SIMPSONS_TITLE_ID
#define SIMPSONS_TITLE_ID "NPUB30563"
#endif

#ifndef SIMPSONS_GAME_DIR
#define SIMPSONS_GAME_DIR "game"
#endif

/* Window defaults (arcade is 4:3; window is 720p letterboxed) */
#define SIMPSONS_WINDOW_WIDTH   1280
#define SIMPSONS_WINDOW_HEIGHT  720
#define SIMPSONS_WINDOW_TITLE   "The Simpsons Arcade Game"

/* ---- ELF layout (from docs/binary-analysis.md) ----
 * e_entry is an OPD (function descriptor) living in the data segment:
 *   real entry code = vm_read32(ENTRY_OPD), TOC = vm_read32(ENTRY_OPD + 4)
 */
#define SIMPSONS_ENTRY_OPD      0x00186900   /* e_entry (OPD) */
#define SIMPSONS_TEXT_BASE      0x00010000   /* PT_LOAD #0 (R-X) */
#define SIMPSONS_TEXT_SIZE      0x001628E8
#define SIMPSONS_DATA_BASE      0x00180000   /* PT_LOAD #1 (RW-) */
#define SIMPSONS_DATA_FILESZ    0x0001A804
#define SIMPSONS_DATA_MEMSZ     0x000C1560

/* Memory layout (within the flat 256 MB guest space) ----------------------
 *   0x00010000-0x001728E8  text + rodata
 *   0x00180000-0x00241560  data + bss
 *   0x00280000-0x00280110  main-thread TLS image (TP-0x7000)
 *   0x00300000-0x009F0000  main-thread stack (grows down)
 *   0x00A00000-0x10000000  HLE bump heap
 */
#define SIMPSONS_MAIN_MEM_SIZE  (256ULL * 1024 * 1024)  /* 256 MB XDR */
#define SIMPSONS_STACK_SIZE     (64 * 1024 * 1024)
#define SIMPSONS_STACK_TOP      0x009F0000

/* PT_TLS template (from docs/binary-analysis.md): vaddr, filesz, memsz. */
#define SIMPSONS_TLS_TEMPLATE   0x0018E35C
#define SIMPSONS_TLS_FILESZ     0x00000004
#define SIMPSONS_TLS_MEMSZ      0x00000110
/* PPC64 ELF TLS: r13 (thread pointer) = image_base + 0x7000. */
#define SIMPSONS_TLS_IMG        0x00280000
#define SIMPSONS_TLS_TP         0x00287000

/* Guest heap region for the HLE bump allocator (bypasses CRT malloc) */
#define SIMPSONS_HEAP_BASE      0x00A00000
#define SIMPSONS_HEAP_END       0x10000000

/* Threading */
#define SIMPSONS_MAX_PPU_THREADS 64

#endif /* SIMPSONS_CONFIG_H */
