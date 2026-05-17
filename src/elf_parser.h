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
} ElfContext;

typedef struct {
    uint32_t fsm_table_addr;
    uint8_t  fsm_table_count;
    uint32_t queue_table_addr;
    uint8_t  queue_count;
    uint32_t event_mask_addr;
    uint32_t mempool_table_addr;
    uint8_t  mempool_count;
    uint32_t current_fsm_addr;
} AvrOsSymbolIndex;

int      elf_open(const char *path, ElfContext *ctx);
void     elf_close(ElfContext *ctx);
int      elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx);
uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma);

#endif /* AOD_ELF_PARSER_H */
