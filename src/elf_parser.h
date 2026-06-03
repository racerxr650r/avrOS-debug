/* src/elf_parser.h */
#ifndef AOD_ELF_PARSER_H
#define AOD_ELF_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __linux__
#include <elf.h>
#else
#include "elf.h"
#endif

/* Maximum length of the lowercase AVR device-name string extracted
 * from the `.note.gnu.avr.deviceinfo` ELF note (e.g. "avr128da28").
 * 16 bytes covers every Microchip AVR-Dx/Du/Sd/Ea/Eb part name with
 * room for a trailing NUL.                                            */
#define ELF_DEVICE_NAME_MAX  16u

/* Maximum number of PT_LOAD segments we remember for VMA→LMA
 * translation.  Real avr-gcc executables typically expose 2–6
 * (.text, .data, .rodata-in-mapped-flash, .bss, .fuse, .lock). */
#define ELF_MAX_PT_LOAD      8u

/* One PT_LOAD segment captured at elf_open() time.  Used by
 * elf_phys_flash_byte_addr() to translate a data-space VMA inside the
 * AVR-Dx mapped-flash window to the absolute physical FLASH byte
 * address (LMA) that UPDI must use. */
typedef struct {
    uint32_t vaddr;   /* p_vaddr   */
    uint32_t paddr;   /* p_paddr   */
    uint32_t memsz;   /* p_memsz   */
} ElfLoadSegment;

typedef struct {
    int         fd;
    Elf32_Ehdr  ehdr;
    Elf32_Sym  *symtab;
    size_t      sym_count;
    char       *strtab;
    size_t      strtab_size;
    uint32_t    flash_base;
    uint32_t    flash_size;
    uint32_t    sram_base;
    uint32_t    sram_size;
    /* PT_LOAD segment table (populated in elf_open). */
    ElfLoadSegment loads[ELF_MAX_PT_LOAD];
    unsigned       load_count;
    /* Lowercase device-name string copied from the
     * `.note.gnu.avr.deviceinfo` ELF note descriptor (e.g.
     * "avr128da28").  Empty string when the note is absent — every
     * consumer must treat an empty string as "ELF carries no device
     * identification".                                               */
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
} AvrOsSymbolIndex;

int      elf_open(const char *path, ElfContext *ctx);
void     elf_close(ElfContext *ctx);
int      elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx);
int      elf_has_fsm_symbols(const AvrOsSymbolIndex *idx);
uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma);
/* Translate a data-space VMA to the absolute physical FLASH byte
 * address (LMA) by locating the PT_LOAD segment that contains it and
 * applying p_paddr - p_vaddr.  Returns `vma` unchanged when no
 * segment matches (e.g. unit-test fixtures with no PT_LOAD entries).
 * This is the address UPDI requires for reads via ST_PTR_LONG. */
uint32_t elf_phys_flash_byte_addr(const ElfContext *ctx, uint32_t vma);

#endif /* AOD_ELF_PARSER_H */
