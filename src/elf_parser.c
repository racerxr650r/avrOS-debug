/* src/elf_parser.c — stub (Phase 1 will implement) */
#ifdef __linux__
#include <elf.h>
#else
#include "elf.h"
#endif

#include "elf_parser.h"

int elf_open(ElfContext *ctx, const char *path)
{
    (void)ctx; (void)path;
    return -1;
}

void elf_close(ElfContext *ctx)
{
    (void)ctx;
}

int elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx)
{
    (void)ctx; (void)idx;
    return -1;
}

uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma)
{
    (void)ctx; (void)vma;
    return 0;
}
