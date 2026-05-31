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

/* Memory layout */
#define SIMPSONS_MAIN_MEM_SIZE  (256ULL * 1024 * 1024)  /* 256 MB XDR */
#define SIMPSONS_STACK_SIZE     (64 * 1024 * 1024)

/* Guest heap region for the HLE bump allocator (bypasses CRT malloc) */
#define SIMPSONS_HEAP_BASE      0x00A00000
#define SIMPSONS_HEAP_END       0x10000000

/* Threading */
#define SIMPSONS_MAX_PPU_THREADS 64

#endif /* SIMPSONS_CONFIG_H */
