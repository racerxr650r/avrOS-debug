/* src/elf_parser.h
 *
 * AVR/avrOS ELF + DWARF domain adapter.
 *
 * This module is the single owner of the AVR-Dx/avrOS-specific knowledge
 * needed to read a target ELF: the avrOS system-table symbols, the AVR-Dx
 * mapped-flash VMA→LMA translation, word-vs-byte flash addressing, the
 * Microchip device-info note, and DWARF source-line lookup.  All raw ELF and
 * DWARF format parsing is delegated to elfutils (libelf via the GElf API, and
 * libdw) — so ELF/DWARF format changes are absorbed by the library, not by
 * hand-rolled byte decoding here.
 *
 * Layering: this is a leaf parsing-layer module.  It is consumed by
 * fsm_mapper and main; it never calls upward into UPDI/RSP.  elfutils is an
 * implementation detail and does not leak through this header (the libelf
 * Elf* / libdw Dwarf* handles are held as opaque void*).
 */
#ifndef AOD_ELF_PARSER_H
#define AOD_ELF_PARSER_H

#include <stddef.h>
#include <stdint.h>

/* System <elf.h> supplies the Elf32_xxx, EM_AVR, PT_LOAD, etc. definitions
 * used by this module's consumers (main.c's flash-load path, the test
 * fixtures).  It is always present now that elfutils is a required dependency. */
#include <elf.h>

/* Maximum length of the lowercase AVR device-name string extracted from the
 * `.note.gnu.avr.deviceinfo` ELF note (e.g. "avr128da28").  16 bytes covers
 * every Microchip AVR-Dx/Du/Sd/Ea/Eb part name with room for a NUL. */
#define ELF_DEVICE_NAME_MAX  16u

/* Maximum number of PT_LOAD segments we remember for VMA→LMA translation.
 * Real avr-gcc executables typically expose 2–6 (.text, .data,
 * .rodata-in-mapped-flash, .bss, .fuse, .lock). */
#define ELF_MAX_PT_LOAD      8u

/* One PT_LOAD segment captured at elf_open() time.  Used by
 * elf_phys_flash_byte_addr() to translate a data-space VMA inside the AVR-Dx
 * mapped-flash window to the absolute physical FLASH byte address (LMA) that
 * UPDI must use. */
typedef struct {
    uint32_t vaddr;   /* p_vaddr   */
    uint32_t paddr;   /* p_paddr   */
    uint32_t memsz;   /* p_memsz   */
} ElfLoadSegment;

typedef struct {
    int        fd;         /* ELF file descriptor (open for libelf/libdw life) */
    void      *elf;        /* opaque libelf  Elf*  handle                      */
    void      *dwarf;      /* opaque libdw   Dwarf* handle, NULL if no DWARF   */
    Elf32_Ehdr ehdr;       /* cached ELF header (consumers walk phdrs via fd)  */
    uint32_t  flash_base;
    uint32_t  flash_size;
    uint32_t  sram_base;
    uint32_t  sram_size;
    /* PT_LOAD segment table (populated in elf_open). */
    ElfLoadSegment loads[ELF_MAX_PT_LOAD];
    unsigned       load_count;
    /* Lowercase device-name string copied from the
     * `.note.gnu.avr.deviceinfo` ELF note descriptor (e.g. "avr128da28").
     * Empty string when the note is absent — every consumer must treat an
     * empty string as "ELF carries no device identification". */
    char        device_name[ELF_DEVICE_NAME_MAX];
} ElfContext;

typedef struct {
    uint32_t fsm_table_addr;     /* FLASH byte LMA of FSM_TABLE    */
    uint8_t  fsm_table_count;    /* entries: stride 9              */
    uint32_t queue_table_addr;   /* FLASH byte LMA of QUE_TABLE    */
    uint8_t  queue_count;        /* entries: stride 10             */
    uint32_t event_table_addr;   /* FLASH byte LMA of EVNT_TABLE   */
    uint8_t  event_count;        /* entries: stride 4              */
    uint32_t current_fsm_addr;   /* SRAM VMA of currStateMachine   */
    uint32_t flash_lma_off;      /* delta from a 16-bit mapped-flash
                                  * data-space pointer (0x8000..0xFFFF) to
                                  * the physical FLASH byte (LMA); add it to
                                  * a `char *` value (masked to 16 bits) to
                                  * get the LMA for a UPDI flash-mirror read.
                                  * = LMA - (VMA & 0xFFFF).  0 when unknown. */
} AvrOsSymbolIndex;

/* Open `path`, validate it is an ELF32 EM_AVR object, capture the PT_LOAD
 * segment table + flash/sram bands, extract the device-info note, and open a
 * DWARF handle when debug info is present.  Returns 0 on success, -1 on any
 * failure (and leaves *ctx safe to pass to elf_close()). */
int      elf_open(const char *path, ElfContext *ctx);
void     elf_close(ElfContext *ctx);

/* Locate the avrOS FSM/QUE/EVNT system tables by their boundary symbols and
 * populate *idx with FLASH-LMA addresses + entry counts.  Returns 0. */
int      elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx);
int      elf_has_fsm_symbols(const AvrOsSymbolIndex *idx);

/* Word-address translation: (vma - flash_base) / 2. */
uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma);

/* Translate a data-space VMA to the absolute physical FLASH byte address
 * (LMA) by locating the PT_LOAD segment that contains it and applying
 * p_paddr - p_vaddr.  Returns `vma` unchanged when no segment matches.  This
 * is the address UPDI requires for reads via ST_PTR_LONG. */
uint32_t elf_phys_flash_byte_addr(const ElfContext *ctx, uint32_t vma);

/* ── DWARF source-level accessors (libdw) ───────────────────────────────────
 * Surface source-line information for the future native DAP front-end (the
 * GDB-RSP path delegates DWARF to avr-gdb and does not call these).  Safe to
 * call when the ELF has no debug info: they return -1 and leave outputs
 * untouched. */

/* 1 — the build always links libdw (elfutils is required). */
int elf_dwarf_available(void);

/* Map a code-space byte address to its source file:line.  On success returns
 * 0, copies the (NUL-terminated, truncated to `file_cap`) source path into
 * `file`, and stores the 1-based line in `*line`.  Either output may be NULL.
 * Returns -1 when the ELF lacks DWARF or `addr` has no line-table entry. */
int elf_addr_to_line(const ElfContext *ctx, uint32_t byte_addr,
                     char *file, size_t file_cap, int *line);

/* Resolve the first code-space byte address for `file`:`line` (file matched
 * by basename).  On success returns 0 and stores the address in *byte_addr.
 * Returns -1 when the ELF lacks DWARF or no matching row exists. */
int elf_line_to_addr(const ElfContext *ctx, const char *file, int line,
                     uint32_t *byte_addr);

/* Call-Frame-Information rule for the (canonical) frame address (CFA) at a
 * code-space byte address.  On success returns 0 and stores the DWARF register
 * whose value forms the CFA base in *cfa_reg (AVR: 28 = the Y frame-pointer
 * pair r28:r29, or 32 = SP) and the signed byte offset added to it in
 * *cfa_offset, so CFA = value(cfa_reg) + cfa_offset.  Returns -1 when the ELF
 * lacks `.debug_frame`, `addr` is not covered by an FDE, or the CFA rule is not
 * the simple register+offset form avr-gcc emits.  This is the per-PC half of a
 * stack unwind; the return-address and saved-frame-pointer recovery (AVR's
 * 2-byte word return address, word<->byte PC) live in the caller's unwinder. */
int elf_cfi_cfa(const ElfContext *ctx, uint32_t byte_addr,
                int *cfa_reg, int *cfa_offset);

#endif /* AOD_ELF_PARSER_H */
