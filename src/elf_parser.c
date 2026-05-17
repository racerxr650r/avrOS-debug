/* src/elf_parser.c — ELF parser implementation (Phase 1) */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#  include <elf.h>
#else
#  include "elf.h"
#endif

#include "elf_parser.h"

/* ── elf_open ────────────────────────────────────────────────────────────── */
int elf_open(const char *path, ElfContext *ctx)
{
    ctx->fd          = -1;
    ctx->symtab      = NULL;
    ctx->sym_count   = 0;
    ctx->strtab      = NULL;
    ctx->strtab_size = 0;
    ctx->flash_base  = 0;
    ctx->flash_size  = 0;
    ctx->sram_base   = 0;
    ctx->sram_size   = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "elf_open: cannot open '%s'\n", path);
        return -1;
    }

    /* Read the ELF file header */
    Elf32_Ehdr ehdr;
    if (read(fd, &ehdr, sizeof(ehdr)) != (ssize_t)sizeof(ehdr)) {
        fprintf(stderr, "elf_open: short read on ELF header in '%s'\n", path);
        close(fd);
        return -1;
    }

    /* Validate magic */
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "elf_open: '%s' is not an ELF file\n", path);
        close(fd);
        return -1;
    }

    /* Validate ELF32 */
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS32) {
        fprintf(stderr, "elf_open: '%s' is not ELF32 (class=%u)\n",
                path, (unsigned)ehdr.e_ident[EI_CLASS]);
        close(fd);
        return -1;
    }

    /* Validate AVR target */
    if (ehdr.e_machine != EM_AVR) {
        fprintf(stderr,
                "elf_open: '%s' has unsupported machine 0x%04x"
                " (expected EM_AVR 0x%04x)\n",
                path, (unsigned)ehdr.e_machine, (unsigned)EM_AVR);
        close(fd);
        return -1;
    }

    ctx->ehdr = ehdr;

    /* Scan PT_LOAD segments: first LOAD → FLASH, second LOAD → SRAM */
    int load_idx = 0;
    for (int i = 0; i < (int)ehdr.e_phnum; i++) {
        Elf32_Phdr phdr;
        off_t off = (off_t)ehdr.e_phoff + (off_t)(i * (int)sizeof(Elf32_Phdr));
        if (lseek(fd, off, SEEK_SET) < 0 ||
            read(fd, &phdr, sizeof(phdr)) != (ssize_t)sizeof(phdr)) {
            fprintf(stderr, "elf_open: failed to read phdr %d\n", i);
            close(fd);
            return -1;
        }
        if (phdr.p_type == PT_LOAD) {
            if (load_idx == 0) {
                ctx->flash_base = phdr.p_vaddr;
                ctx->flash_size = phdr.p_filesz;
            } else if (load_idx == 1) {
                ctx->sram_base = phdr.p_vaddr;
                ctx->sram_size = phdr.p_filesz;
            }
            load_idx++;
        }
    }

    /* Find the SHT_SYMTAB section */
    Elf32_Shdr symtab_shdr;
    int found_symtab = 0;
    for (int i = 0; i < (int)ehdr.e_shnum; i++) {
        Elf32_Shdr shdr;
        off_t off = (off_t)ehdr.e_shoff + (off_t)(i * (int)sizeof(Elf32_Shdr));
        if (lseek(fd, off, SEEK_SET) < 0 ||
            read(fd, &shdr, sizeof(shdr)) != (ssize_t)sizeof(shdr)) {
            fprintf(stderr, "elf_open: failed to read shdr %d\n", i);
            close(fd);
            return -1;
        }
        if (shdr.sh_type == SHT_SYMTAB) {
            symtab_shdr = shdr;
            found_symtab = 1;
            break;
        }
    }

    if (!found_symtab) {
        fprintf(stderr, "elf_open: no .symtab in '%s'\n", path);
        close(fd);
        return -1;
    }

    /* Allocate and load .symtab */
    ctx->sym_count = symtab_shdr.sh_size / sizeof(Elf32_Sym);
    ctx->symtab    = malloc(symtab_shdr.sh_size);
    if (!ctx->symtab) {
        fprintf(stderr, "elf_open: malloc failed for .symtab\n");
        close(fd);
        return -1;
    }
    if (lseek(fd, (off_t)symtab_shdr.sh_offset, SEEK_SET) < 0 ||
        read(fd, ctx->symtab, symtab_shdr.sh_size) !=
            (ssize_t)symtab_shdr.sh_size) {
        fprintf(stderr, "elf_open: failed to read .symtab\n");
        free(ctx->symtab);
        ctx->symtab    = NULL;
        ctx->sym_count = 0;
        close(fd);
        return -1;
    }

    /* Load .strtab via sh_link on the .symtab section header */
    Elf32_Shdr strtab_shdr;
    off_t strtab_hdr_off = (off_t)ehdr.e_shoff
                         + (off_t)((int)symtab_shdr.sh_link
                                   * (int)sizeof(Elf32_Shdr));
    if (lseek(fd, strtab_hdr_off, SEEK_SET) < 0 ||
        read(fd, &strtab_shdr, sizeof(strtab_shdr)) !=
            (ssize_t)sizeof(strtab_shdr)) {
        fprintf(stderr, "elf_open: failed to read .strtab section header\n");
        free(ctx->symtab);
        ctx->symtab    = NULL;
        ctx->sym_count = 0;
        close(fd);
        return -1;
    }

    ctx->strtab_size = strtab_shdr.sh_size;
    ctx->strtab      = malloc(strtab_shdr.sh_size);
    if (!ctx->strtab) {
        fprintf(stderr, "elf_open: malloc failed for .strtab\n");
        free(ctx->symtab);
        ctx->symtab      = NULL;
        ctx->sym_count   = 0;
        ctx->strtab_size = 0;
        close(fd);
        return -1;
    }
    if (lseek(fd, (off_t)strtab_shdr.sh_offset, SEEK_SET) < 0 ||
        read(fd, ctx->strtab, strtab_shdr.sh_size) !=
            (ssize_t)strtab_shdr.sh_size) {
        fprintf(stderr, "elf_open: failed to read .strtab\n");
        free(ctx->strtab);
        ctx->strtab      = NULL;
        ctx->strtab_size = 0;
        free(ctx->symtab);
        ctx->symtab    = NULL;
        ctx->sym_count = 0;
        close(fd);
        return -1;
    }

    ctx->fd = fd;
    return 0;
}

/* ── elf_close ───────────────────────────────────────────────────────────── */
void elf_close(ElfContext *ctx)
{
    free(ctx->symtab);
    ctx->symtab    = NULL;
    ctx->sym_count = 0;

    free(ctx->strtab);
    ctx->strtab      = NULL;
    ctx->strtab_size = 0;

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

/* ── elf_find_avros_tables ───────────────────────────────────────────────── */
int elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx)
{
    /* Accumulate _start and _end VMAs in a single O(n) pass.
     * Symbol names match the real avrOS linker script
     * (app/avrOS_example/avrOS.x): FSM_TABLE, QUE_TABLE, EVNT_TABLE
     * sections with __start_<NAME> / __stop_<NAME> boundary symbols, plus
     * the file-static `currStateMachine` (STB_LOCAL but still in .symtab). */
    uint32_t fsm_start   = 0, fsm_end   = 0;
    uint32_t queue_start = 0, queue_end = 0;
    uint32_t event_start = 0, event_end = 0;

    /* Bitmask to track which sentinels were found */
    unsigned int found = 0U;
#define B_FSM_S  0x01U
#define B_FSM_E  0x02U
#define B_Q_S    0x04U
#define B_Q_E    0x08U
#define B_EVT_S  0x10U
#define B_EVT_E  0x20U
#define B_CURR   0x40U
#define B_ALL    0x7FU

    for (size_t i = 0; i < ctx->sym_count; i++) {
        const Elf32_Sym *sym = &ctx->symtab[i];

        if (sym->st_shndx == SHN_UNDEF || sym->st_name == 0)
            continue;
        if ((size_t)sym->st_name >= ctx->strtab_size)
            continue;

        const char *name = ctx->strtab + sym->st_name;

        if      (!strcmp(name, "__start_FSM_TABLE"))
            { fsm_start   = sym->st_value; found |= B_FSM_S; }
        else if (!strcmp(name, "__stop_FSM_TABLE"))
            { fsm_end     = sym->st_value; found |= B_FSM_E; }
        else if (!strcmp(name, "__start_QUE_TABLE"))
            { queue_start = sym->st_value; found |= B_Q_S;   }
        else if (!strcmp(name, "__stop_QUE_TABLE"))
            { queue_end   = sym->st_value; found |= B_Q_E;   }
        else if (!strcmp(name, "__start_EVNT_TABLE"))
            { event_start = sym->st_value; found |= B_EVT_S; }
        else if (!strcmp(name, "__stop_EVNT_TABLE"))
            { event_end   = sym->st_value; found |= B_EVT_E; }
        else if (!strcmp(name, "currStateMachine"))
            { idx->current_fsm_addr = sym->st_value; found |= B_CURR; }
    }

    /* Compute word addresses and entry counts from gathered VMAs.
     * Per-entry strides match the real avrOS struct layouts:
     *   fsmStateMachineDescr_t  = 9 bytes
     *   queDescriptor_t (no QUE_STATS) = 10 bytes
     *   evntDescriptor_t        = 4 bytes
     */
    if (found & B_FSM_S)
        idx->fsm_table_addr  = elf_flash_addr(ctx, fsm_start);
    if ((found & (B_FSM_S | B_FSM_E)) == (B_FSM_S | B_FSM_E))
        idx->fsm_table_count = (uint8_t)((fsm_end - fsm_start) / 9U);

    if (found & B_Q_S)
        idx->queue_table_addr = elf_flash_addr(ctx, queue_start);
    if ((found & (B_Q_S | B_Q_E)) == (B_Q_S | B_Q_E))
        idx->queue_count = (uint8_t)((queue_end - queue_start) / 10U);

    if (found & B_EVT_S)
        idx->event_table_addr = elf_flash_addr(ctx, event_start);
    if ((found & (B_EVT_S | B_EVT_E)) == (B_EVT_S | B_EVT_E))
        idx->event_count = (uint8_t)((event_end - event_start) / 4U);

    /* Log any missing sentinels */
    if (found != B_ALL) {
        if (!(found & B_FSM_S))  fprintf(stderr, "elf: missing __start_FSM_TABLE\n");
        if (!(found & B_FSM_E))  fprintf(stderr, "elf: missing __stop_FSM_TABLE\n");
        if (!(found & B_Q_S))    fprintf(stderr, "elf: missing __start_QUE_TABLE\n");
        if (!(found & B_Q_E))    fprintf(stderr, "elf: missing __stop_QUE_TABLE\n");
        if (!(found & B_EVT_S))  fprintf(stderr, "elf: missing __start_EVNT_TABLE\n");
        if (!(found & B_EVT_E))  fprintf(stderr, "elf: missing __stop_EVNT_TABLE\n");
        if (!(found & B_CURR))   fprintf(stderr, "elf: missing currStateMachine\n");
    }

    return 0;
}

/* ── elf_flash_addr ──────────────────────────────────────────────────────── */
uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma)
{
    return (vma - ctx->flash_base) / 2U;
}

