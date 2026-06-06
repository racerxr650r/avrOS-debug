/* src/elf_parser.c — AVR/avrOS ELF + DWARF domain adapter over elfutils.
 *
 * Raw ELF parsing is delegated to libelf (via the class-independent GElf
 * API); DWARF line lookup to libdw.  This file holds only the AVR-Dx/avrOS
 * domain logic on top of those: PT_LOAD flash/sram capture, the mapped-flash
 * VMA→LMA translation, word-vs-byte flash addressing, the device-info note,
 * and the avrOS system-table symbol discovery.
 *
 * Layering: leaf parsing-layer module.  Consumed by fsm_mapper and main; it
 * never calls upward into UPDI/RSP.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gelf.h>               /* libelf class-independent API + <elf.h> */
#include <elfutils/libdw.h>     /* DWARF */
#include <dwarf.h>

#include "elf_parser.h"

/* ── deviceinfo note scan (internal) ───────────────────────────────────────
 * Microchip's avr-gcc emits a `.note.gnu.avr.deviceinfo` ELF note carrying
 * the exact lowercase device-name string ("avr128da28", "avr64dd32", …).  The
 * descriptor layout has drifted across binutils versions, so rather than
 * decode the struct we scan the note section bytes for the first
 * NUL-terminated ASCII run that begins with "avr" followed by a digit — this
 * survives format drift while matching every device-pack-generated ELF.
 * Leaves device_name the empty string on any miss (caller pre-cleared it). */
static void elf_scan_deviceinfo_note(Elf *e, ElfContext *ctx)
{
    Elf_Scn *scn = NULL;
    while ((scn = elf_nextscn(e, scn)) != NULL) {
        GElf_Shdr sh;
        if (gelf_getshdr(scn, &sh) == NULL)  continue;
        if (sh.sh_type != SHT_NOTE)          continue;

        Elf_Data *d = elf_getdata(scn, NULL);
        if (d == NULL || d->d_buf == NULL)   continue;

        const unsigned char *buf = d->d_buf;
        const size_t         n   = d->d_size;
        for (size_t k = 0; k + 4u < n; k++) {
            if (buf[k] != 'a' || buf[k+1] != 'v' || buf[k+2] != 'r')
                continue;
            if (buf[k+3] < '0' || buf[k+3] > '9')
                continue;
            size_t j = 0;
            while (k + j < n
                   && buf[k+j] >= 0x20 && buf[k+j] < 0x7F
                   && j + 1u < sizeof ctx->device_name) {
                ctx->device_name[j] = (char)buf[k+j];
                j++;
            }
            ctx->device_name[j] = '\0';
            return;
        }
    }
}

/* ── elf_open ──────────────────────────────────────────────────────────── */
int elf_open(const char *path, ElfContext *ctx)
{
    ctx->fd          = -1;
    ctx->elf         = NULL;
    ctx->dwarf       = NULL;
    memset(&ctx->ehdr, 0, sizeof ctx->ehdr);
    ctx->flash_base  = 0;
    ctx->flash_size  = 0;
    ctx->sram_base   = 0;
    ctx->sram_size   = 0;
    ctx->load_count  = 0;
    ctx->device_name[0] = '\0';

    if (elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "elf_open: libelf version mismatch: %s\n",
                elf_errmsg(-1));
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "elf_open: cannot open '%s'\n", path);
        return -1;
    }

    Elf *e = elf_begin(fd, ELF_C_READ, NULL);
    if (e == NULL) {
        fprintf(stderr, "elf_open: '%s' is not readable as ELF: %s\n",
                path, elf_errmsg(-1));
        close(fd);
        return -1;
    }

    if (elf_kind(e) != ELF_K_ELF) {
        fprintf(stderr, "elf_open: '%s' is not an ELF object\n", path);
        goto fail;
    }
    if (gelf_getclass(e) != ELFCLASS32) {
        fprintf(stderr, "elf_open: '%s' is not ELF32\n", path);
        goto fail;
    }

    /* elf32_getehdr() returns the native 32-bit header directly (the object
     * is ELF32 by the class check above) — cache it for consumers that walk
     * program headers straight from the fd (main.c's flash-load path). */
    Elf32_Ehdr *e32 = elf32_getehdr(e);
    if (e32 == NULL) {
        fprintf(stderr, "elf_open: '%s' has no ELF header: %s\n",
                path, elf_errmsg(-1));
        goto fail;
    }
    if (e32->e_machine != EM_AVR) {
        fprintf(stderr,
                "elf_open: '%s' has unsupported machine 0x%04x"
                " (expected EM_AVR 0x%04x)\n",
                path, (unsigned)e32->e_machine, (unsigned)EM_AVR);
        goto fail;
    }
    ctx->ehdr = *e32;

    /* PT_LOAD segments: first LOAD → FLASH, second LOAD → SRAM. */
    size_t phnum = 0;
    if (elf_getphdrnum(e, &phnum) == 0) {
        int load_idx = 0;
        for (size_t i = 0; i < phnum; i++) {
            GElf_Phdr ph;
            if (gelf_getphdr(e, (int)i, &ph) == NULL)  continue;
            if (ph.p_type != PT_LOAD)                  continue;
            if (load_idx == 0) {
                ctx->flash_base = (uint32_t)ph.p_vaddr;
                ctx->flash_size = (uint32_t)ph.p_filesz;
            } else if (load_idx == 1) {
                ctx->sram_base = (uint32_t)ph.p_vaddr;
                ctx->sram_size = (uint32_t)ph.p_filesz;
            }
            if (ctx->load_count < ELF_MAX_PT_LOAD) {
                ctx->loads[ctx->load_count].vaddr = (uint32_t)ph.p_vaddr;
                ctx->loads[ctx->load_count].paddr = (uint32_t)ph.p_paddr;
                ctx->loads[ctx->load_count].memsz = (uint32_t)ph.p_memsz;
                ctx->load_count++;
            }
            load_idx++;
        }
    }

    /* Best-effort device name from the deviceinfo note. */
    elf_scan_deviceinfo_note(e, ctx);

    /* Best-effort DWARF handle (NULL when the ELF has no debug info).  Shares
     * the libelf Elf*, so dwarf_end() must precede elf_end() in elf_close(). */
    ctx->dwarf = dwarf_begin_elf(e, DWARF_C_READ, NULL);

    ctx->elf = e;
    ctx->fd  = fd;
    return 0;

fail:
    elf_end(e);
    close(fd);
    return -1;
}

/* ── elf_close ─────────────────────────────────────────────────────────── */
void elf_close(ElfContext *ctx)
{
    if (ctx->dwarf != NULL) {
        dwarf_end((Dwarf *)ctx->dwarf);
        ctx->dwarf = NULL;
    }
    if (ctx->elf != NULL) {
        elf_end((Elf *)ctx->elf);
        ctx->elf = NULL;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

/* ── elf_find_avros_tables ─────────────────────────────────────────────────
 * Single O(n) pass over the first .symtab.  Symbol names match the avrOS
 * linker script (app/avrOS_example/avrOS.x): FSM_TABLE/QUE_TABLE/EVNT_TABLE
 * sections with __start_<NAME>/__stop_<NAME> boundary symbols, plus the
 * file-static `currStateMachine`. */
int elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx)
{
    uint32_t fsm_start   = 0, fsm_end   = 0;
    uint32_t queue_start = 0, queue_end = 0;
    uint32_t event_start = 0, event_end = 0;

    unsigned int found = 0U;
#define B_FSM_S  0x01U
#define B_FSM_E  0x02U
#define B_Q_S    0x04U
#define B_Q_E    0x08U
#define B_EVT_S  0x10U
#define B_EVT_E  0x20U
#define B_CURR   0x40U
#define B_ALL    0x7FU

    Elf *e = (Elf *)ctx->elf;
    Elf_Scn *scn = NULL;
    while (e != NULL && (scn = elf_nextscn(e, scn)) != NULL) {
        GElf_Shdr sh;
        if (gelf_getshdr(scn, &sh) == NULL)  continue;
        if (sh.sh_type != SHT_SYMTAB)        continue;

        Elf_Data *d = elf_getdata(scn, NULL);
        if (d == NULL)                       break;
        size_t count = (sh.sh_entsize != 0) ? sh.sh_size / sh.sh_entsize : 0;

        for (size_t i = 0; i < count; i++) {
            GElf_Sym sym;
            if (gelf_getsym(d, (int)i, &sym) == NULL)  continue;
            if (sym.st_shndx == SHN_UNDEF || sym.st_name == 0)  continue;

            const char *name = elf_strptr(e, sh.sh_link, sym.st_name);
            if (name == NULL)  continue;
            uint32_t val = (uint32_t)sym.st_value;

            if      (!strcmp(name, "__start_FSM_TABLE"))
                { fsm_start   = val; found |= B_FSM_S; }
            else if (!strcmp(name, "__stop_FSM_TABLE"))
                { fsm_end     = val; found |= B_FSM_E; }
            else if (!strcmp(name, "__start_QUE_TABLE"))
                { queue_start = val; found |= B_Q_S;   }
            else if (!strcmp(name, "__stop_QUE_TABLE"))
                { queue_end   = val; found |= B_Q_E;   }
            else if (!strcmp(name, "__start_EVNT_TABLE"))
                { event_start = val; found |= B_EVT_S; }
            else if (!strcmp(name, "__stop_EVNT_TABLE"))
                { event_end   = val; found |= B_EVT_E; }
            else if (!strcmp(name, "currStateMachine"))
                { idx->current_fsm_addr = val; found |= B_CURR; }
        }
        break;  /* first symtab only */
    }

    /* Translate each table VMA (in the AVR-Dx mapped-flash window) to its
     * physical FLASH byte (LMA) via the PT_LOAD p_paddr basis — the same
     * translation the loader uses — and record the window's LMA-VMA delta so
     * fsm_mapper can translate the per-entry name pointers it reads at runtime.
     * Per-entry strides match the avrOS struct layouts (9 / 10 / 4 bytes). */
    if (found & B_FSM_S) {
        idx->fsm_table_addr = elf_phys_flash_byte_addr(ctx, fsm_start);
        idx->flash_lma_off  = idx->fsm_table_addr - fsm_start;
    }
    if ((found & (B_FSM_S | B_FSM_E)) == (B_FSM_S | B_FSM_E))
        idx->fsm_table_count = (uint8_t)((fsm_end - fsm_start) / 9U);

    if (found & B_Q_S)
        idx->queue_table_addr = elf_phys_flash_byte_addr(ctx, queue_start);
    if ((found & (B_Q_S | B_Q_E)) == (B_Q_S | B_Q_E))
        idx->queue_count = (uint8_t)((queue_end - queue_start) / 10U);

    if (found & B_EVT_S)
        idx->event_table_addr = elf_phys_flash_byte_addr(ctx, event_start);
    if ((found & (B_EVT_S | B_EVT_E)) == (B_EVT_S | B_EVT_E))
        idx->event_count = (uint8_t)((event_end - event_start) / 4U);

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

int elf_has_fsm_symbols(const AvrOsSymbolIndex *idx)
{
    return idx != NULL
        && idx->fsm_table_addr != 0u
        && idx->fsm_table_count != 0u;
}

/* ── elf_flash_addr ────────────────────────────────────────────────────── */
uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma)
{
    return (vma - ctx->flash_base) / 2U;
}

/* ── elf_phys_flash_byte_addr ──────────────────────────────────────────── */
uint32_t elf_phys_flash_byte_addr(const ElfContext *ctx, uint32_t vma)
{
    for (unsigned i = 0; i < ctx->load_count; i++) {
        const ElfLoadSegment *s = &ctx->loads[i];
        if (vma >= s->vaddr && vma < s->vaddr + s->memsz) {
            return vma + (s->paddr - s->vaddr);
        }
    }
    return vma;
}

/* ── DWARF source-level accessors ──────────────────────────────────────── */
int elf_dwarf_available(void)
{
    return 1;   /* elfutils is a required dependency; libdw is always linked */
}

int elf_addr_to_line(const ElfContext *ctx, uint32_t byte_addr,
                     char *file, size_t file_cap, int *line)
{
    if (ctx == NULL || ctx->dwarf == NULL)
        return -1;

    Dwarf    *dw = (Dwarf *)ctx->dwarf;
    Dwarf_Die cu;
    if (dwarf_addrdie(dw, (Dwarf_Addr)byte_addr, &cu) == NULL)
        return -1;

    Dwarf_Line *ln = dwarf_getsrc_die(&cu, (Dwarf_Addr)byte_addr);
    if (ln == NULL)
        return -1;

    if (line != NULL) {
        int n = 0;
        if (dwarf_lineno(ln, &n) != 0)
            return -1;
        *line = n;
    }
    if (file != NULL && file_cap > 0) {
        const char *src = dwarf_linesrc(ln, NULL, NULL);
        if (src == NULL)
            return -1;
        strncpy(file, src, file_cap - 1);
        file[file_cap - 1] = '\0';
    }
    return 0;
}

int elf_line_to_addr(const ElfContext *ctx, const char *want_file,
                     int want_line, uint32_t *byte_addr)
{
    if (ctx == NULL || ctx->dwarf == NULL || want_file == NULL)
        return -1;

    Dwarf      *dw = (Dwarf *)ctx->dwarf;
    const char *want_base = strrchr(want_file, '/');
    want_base = (want_base != NULL) ? want_base + 1 : want_file;

    Dwarf_Off off = 0, next_off = 0;
    size_t    hdr_size = 0;
    while (dwarf_nextcu(dw, off, &next_off, &hdr_size,
                        NULL, NULL, NULL) == 0) {
        Dwarf_Die cu;
        if (dwarf_offdie(dw, off + hdr_size, &cu) != NULL) {
            Dwarf_Lines *lines  = NULL;
            size_t       nlines = 0;
            if (dwarf_getsrclines(&cu, &lines, &nlines) == 0) {
                for (size_t i = 0; i < nlines; i++) {
                    Dwarf_Line *ln = dwarf_onesrcline(lines, i);
                    if (ln == NULL)  continue;
                    int n = 0;
                    if (dwarf_lineno(ln, &n) != 0 || n != want_line)  continue;
                    const char *src = dwarf_linesrc(ln, NULL, NULL);
                    if (src == NULL)  continue;
                    const char *base = strrchr(src, '/');
                    base = (base != NULL) ? base + 1 : src;
                    if (strcmp(base, want_base) != 0)  continue;
                    Dwarf_Addr a = 0;
                    if (dwarf_lineaddr(ln, &a) != 0)  continue;
                    if (byte_addr != NULL)
                        *byte_addr = (uint32_t)a;
                    return 0;
                }
            }
        }
        off = next_off;
    }
    return -1;
}
