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
     * Per-entry strides match the avrOS struct layouts (9 / 10 / 4 bytes).
     *
     * `flash_lma_off` is added at runtime to a *16-bit* AVR C pointer (the
     * mapped-flash data-space value 0x8000..0xFFFF stored in `char *`
     * fields), so it must be the delta from that 16-bit value to the
     * physical FLASH LMA — i.e. `LMA - (VMA & 0xFFFF)` — NOT `LMA - VMA`.
     * With avr-gcc 14.2 `.rodata` carries a full mapped-flash VMA
     * (e.g. 0x00a08000), so subtracting the full VMA yielded a bogus
     * (negative) delta and every FSM name / `currStateName` string read
     * landed in the wrong FLASH page.  Masking to 16 bits is correct for
     * both the new full-VMA convention and any 16-bit-VMA fixture.       */
    if (found & B_FSM_S) {
        idx->fsm_table_addr = elf_phys_flash_byte_addr(ctx, fsm_start);
        idx->flash_lma_off  = idx->fsm_table_addr - (fsm_start & 0xFFFFu);
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

/* ── .debug_frame CFI (CFA-rule) parser ──────────────────────────────────────
 *
 * libdw's high-level CFI executor (dwarf_cfi_addrframe) returns UNKNOWN_ERROR
 * on AVR `.debug_frame` — GDB likewise ships its own CFI interpreter rather
 * than relying on libdw for this target.  The CFA-defining slice of the CFI
 * instruction set is tiny, so we run our own minimal state machine: enough to
 * recover CFA = value(cfa_reg) + cfa_offset at a PC.  Register-save rules
 * (DW_CFA_offset &c.) are parsed only far enough to skip their operands — the
 * return-address / saved-frame-pointer recovery lives in the unwinder, which
 * applies AVR's fixed stack conventions. */

static uint64_t cfi_uleb(const uint8_t **pp, const uint8_t *end)
{
    uint64_t v = 0; int s = 0; const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << s;
        s += 7;
        if (!(b & 0x80)) break;
    }
    *pp = p;
    return v;
}

static int64_t cfi_sleb(const uint8_t **pp, const uint8_t *end)
{
    int64_t v = 0; int s = 0; uint8_t b = 0; const uint8_t *p = *pp;
    while (p < end) {
        b = *p++;
        v |= (int64_t)(b & 0x7f) << s;
        s += 7;
        if (!(b & 0x80)) break;
    }
    if (s < 64 && (b & 0x40)) v |= -((int64_t)1 << s);
    *pp = p;
    return v;
}

static uint32_t cfi_read_n(const uint8_t **pp, unsigned n)
{
    const uint8_t *p = *pp; uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    *pp += n;
    return v;
}

typedef struct {
    uint8_t        code_align;     /* code alignment factor (ULEB)            */
    int8_t         addr_size;      /* address size in bytes (4 on AVR ELF32)  */
    const uint8_t *insn, *insn_end;/* initial instruction range               */
} CfiCie;

/* Run the CIE-initial then FDE instructions, advancing `loc` until the next
 * advance would pass `pc`; track only the CFA rule.  Returns 0 if a CFA rule
 * was established by then, -1 otherwise. */
static int cfi_run(const CfiCie *cie, const uint8_t *fde, const uint8_t *fde_end,
                   uint32_t init_loc, uint32_t pc, int *cfa_reg, int *cfa_offset)
{
    uint32_t loc = init_loc;
    int reg = -1; long off = 0; bool have = false;

    for (int pass = 0; pass < 2; pass++) {
        const uint8_t *p   = pass ? fde : cie->insn;
        const uint8_t *end = pass ? fde_end : cie->insn_end;
        while (p < end) {
            uint8_t op = *p++;
            uint8_t hi = op & 0xc0, lo = op & 0x3f;
            if (hi == DW_CFA_advance_loc) {
                if (loc + (uint32_t)lo * cie->code_align > pc) goto done;
                loc += (uint32_t)lo * cie->code_align;
            } else if (hi == DW_CFA_offset) {           /* reg=lo, ULEB off  */
                (void)cfi_uleb(&p, end);
            } else if (hi == DW_CFA_restore) {           /* reg=lo, no operand*/
                /* nothing */
            } else switch (lo) {                         /* hi == 0 (extended)*/
                case DW_CFA_nop: break;
                case DW_CFA_set_loc: {
                    uint32_t a = cfi_read_n(&p, (unsigned)cie->addr_size);
                    if (a > pc) goto done;
                    loc = a;
                    break; }
                case DW_CFA_advance_loc1: case DW_CFA_advance_loc2:
                case DW_CFA_advance_loc4: {
                    unsigned n = (lo == DW_CFA_advance_loc1) ? 1u
                               : (lo == DW_CFA_advance_loc2) ? 2u : 4u;
                    uint32_t d = cfi_read_n(&p, n) * cie->code_align;
                    if (loc + d > pc) goto done;
                    loc += d;
                    break; }
                case DW_CFA_def_cfa:
                    reg = (int)cfi_uleb(&p, end);
                    off = (long)cfi_uleb(&p, end); have = true; break;
                case DW_CFA_def_cfa_register:
                    reg = (int)cfi_uleb(&p, end); have = true; break;
                case DW_CFA_def_cfa_offset:
                    off = (long)cfi_uleb(&p, end); break;
                case DW_CFA_def_cfa_sf:
                    reg = (int)cfi_uleb(&p, end);
                    off = (long)cfi_sleb(&p, end); have = true; break;
                case DW_CFA_def_cfa_offset_sf:
                    off = (long)cfi_sleb(&p, end); break;
                /* register-save / misc rules: consume operands, ignore */
                case DW_CFA_offset_extended:
                case DW_CFA_register:
                case DW_CFA_val_offset:
                    (void)cfi_uleb(&p, end); (void)cfi_uleb(&p, end); break;
                case DW_CFA_offset_extended_sf:
                case DW_CFA_val_offset_sf:
                    (void)cfi_uleb(&p, end); (void)cfi_sleb(&p, end); break;
                case DW_CFA_restore_extended:
                case DW_CFA_undefined:
                case DW_CFA_same_value:
                    (void)cfi_uleb(&p, end); break;
                case DW_CFA_remember_state:
                case DW_CFA_restore_state:
                    break;  /* AVR prologues don't alter the CFA across these */
                case DW_CFA_expression: case DW_CFA_val_expression: {
                    (void)cfi_uleb(&p, end);
                    uint64_t len = cfi_uleb(&p, end); p += len; break; }
                case DW_CFA_def_cfa_expression: {
                    uint64_t len = cfi_uleb(&p, end); p += len;
                    have = false; reg = -1; break; }  /* unsupported CFA form */
                default: goto done;  /* unknown opcode — stop conservatively */
            }
        }
    }
done:
    if (!have || reg < 0) return -1;
    if (cfa_reg    != NULL) *cfa_reg    = reg;
    if (cfa_offset != NULL) *cfa_offset = (int)off;
    return 0;
}

/* Locate and parse the CIE at section offset `cie_off`; fill *out.  Returns 0
 * on success. */
static int cfi_parse_cie(const uint8_t *base, size_t size, size_t cie_off,
                         CfiCie *out)
{
    if (cie_off + 4 > size) return -1;
    const uint8_t *p = base + cie_off;
    uint32_t len = cfi_read_n(&p, 4);
    if (len == 0xffffffffu || len == 0) return -1;       /* 64-bit DWARF: n/a */
    const uint8_t *end = p + len;
    if (end > base + size) return -1;
    (void)cfi_read_n(&p, 4);                              /* CIE_id (0xffff…) */
    uint8_t version = *p++;
    const char *aug = (const char *)p;
    while (p < end && *p) p++;                            /* augmentation str */
    if (p < end) p++;                                     /* NUL              */
    out->addr_size = 4;                                   /* AVR ELF32        */
    if (version >= 4) { out->addr_size = (int8_t)*p++; p++; /* addr+seg size  */ }
    out->code_align = (uint8_t)cfi_uleb(&p, end);
    (void)cfi_sleb(&p, end);                              /* data align factor*/
    if (version == 1) p++;                                /* RA reg (1 byte)  */
    else (void)cfi_uleb(&p, end);                         /* RA reg (ULEB)    */
    if (aug[0] == 'z') return -1;       /* augmentation data present: skip CIE*/
    out->insn = p; out->insn_end = end;
    return 0;
}

/* See elf_parser.h. */
int elf_cfi_cfa(const ElfContext *ctx, uint32_t byte_addr,
                int *cfa_reg, int *cfa_offset)
{
    if (ctx == NULL || ctx->elf == NULL)
        return -1;

    Elf     *e = (Elf *)ctx->elf;
    size_t   shstrndx = 0;
    if (elf_getshdrstrndx(e, &shstrndx) != 0)
        return -1;

    Elf_Data *data = NULL;
    Elf_Scn  *scn  = NULL;
    while ((scn = elf_nextscn(e, scn)) != NULL) {
        GElf_Shdr sh;
        if (gelf_getshdr(scn, &sh) == NULL) continue;
        const char *nm = elf_strptr(e, shstrndx, sh.sh_name);
        if (nm != NULL && strcmp(nm, ".debug_frame") == 0) {
            data = elf_getdata(scn, NULL);
            break;
        }
    }
    if (data == NULL || data->d_buf == NULL || data->d_size == 0)
        return -1;

    const uint8_t *base = data->d_buf;
    size_t         size = data->d_size;

    /* Walk entries; on the FDE covering byte_addr, parse its CIE and run. */
    size_t off = 0;
    while (off + 8 <= size) {
        const uint8_t *p   = base + off;
        uint32_t       len = cfi_read_n(&p, 4);
        if (len == 0xffffffffu || len == 0) break;       /* 64-bit / padding */
        size_t entry_end = (size_t)(p - base) + len;
        if (entry_end > size) break;
        uint32_t cie_id = cfi_read_n(&p, 4);
        if (cie_id != 0xffffffffu) {                      /* FDE              */
            CfiCie cie;
            if (cfi_parse_cie(base, size, (size_t)cie_id, &cie) == 0) {
                uint32_t init_loc = cfi_read_n(&p, (unsigned)cie.addr_size);
                uint32_t range    = cfi_read_n(&p, (unsigned)cie.addr_size);
                if (byte_addr >= init_loc && byte_addr < init_loc + range)
                    return cfi_run(&cie, p, base + entry_end, init_loc,
                                   byte_addr, cfa_reg, cfa_offset);
            }
        }
        off = entry_end;
    }
    return -1;
}

/* Does the subprogram scope (if any) among `scopes` use DW_OP_call_frame_cfa as
 * its frame base?  Needed to resolve DW_OP_fbreg locals against the CFA. */
static bool cfi_frame_base_is_cfa(Dwarf_Die *scopes, int n)
{
    for (int i = 0; i < n; i++) {
        if (dwarf_tag(&scopes[i]) != DW_TAG_subprogram) continue;
        Dwarf_Attribute a;
        if (dwarf_attr(&scopes[i], DW_AT_frame_base, &a) == NULL) return false;
        Dwarf_Op *ops = NULL; size_t nops = 0;
        if (dwarf_getlocation(&a, &ops, &nops) == 0 && nops == 1 &&
            ops[0].atom == DW_OP_call_frame_cfa)
            return true;
        return false;
    }
    return false;
}

/* Fill *size / *is_signed from a variable DIE's DW_AT_type (peeling typedef /
 * const / volatile). Defaults to 2 bytes / unsigned when the type is absent. */
static void var_type_info(Dwarf_Die *var, int *size, bool *is_signed)
{
    if (size != NULL)      *size = 2;
    if (is_signed != NULL) *is_signed = false;
    Dwarf_Attribute ta;
    Dwarf_Die type, peeled;
    if (dwarf_attr_integrate(var, DW_AT_type, &ta) == NULL) return;
    if (dwarf_formref_die(&ta, &type) == NULL) return;
    if (dwarf_peel_type(&type, &peeled) != 0) return;
    int bs = dwarf_bytesize(&peeled);
    if (bs >= 1 && bs <= 4 && size != NULL) *size = bs;
    Dwarf_Attribute ea;
    if (dwarf_attr(&peeled, DW_AT_encoding, &ea) != NULL && is_signed != NULL) {
        Dwarf_Word enc = 0;
        if (dwarf_formudata(&ea, &enc) == 0)
            *is_signed = (enc == DW_ATE_signed || enc == DW_ATE_signed_char);
    }
}

/* Evaluate a variable DIE's single-op DWARF location to a 16-bit data-space
 * address using the live frame registers — the location forms avr-gcc -O0
 * emits: DW_OP_addr (global), DW_OP_breg28/bregx28 (Y pair), DW_OP_bregx32
 * (SP), DW_OP_fbreg (CFA, when the subprogram frame base is call_frame_cfa).
 * Returns 0 on success, -1 for an unsupported form or a missing register. */
static int eval_var_loc(Dwarf_Die *var, const ElfFrameRegs *fr,
                        bool fb_is_cfa, uint32_t *addr)
{
    Dwarf_Attribute la;
    if (dwarf_attr_integrate(var, DW_AT_location, &la) == NULL) return -1;
    Dwarf_Op *ops = NULL; size_t nops = 0;
    if (dwarf_getlocation(&la, &ops, &nops) != 0 || ops == NULL || nops < 1)
        return -1;

    uint32_t a;
    if (ops[0].atom == DW_OP_addr) {
        a = (uint32_t)ops[0].number;
    } else if (ops[0].atom == DW_OP_breg0 + 28) {
        if (fr == NULL) return -1;
        a = fr->y + (uint32_t)(int32_t)ops[0].number;
    } else if (ops[0].atom == DW_OP_bregx && ops[0].number == 28) {
        if (fr == NULL) return -1;
        a = fr->y + (uint32_t)(int32_t)ops[0].number2;
    } else if (ops[0].atom == DW_OP_bregx && ops[0].number == 32) {
        if (fr == NULL) return -1;
        a = fr->sp + (uint32_t)(int32_t)ops[0].number2;
    } else if (ops[0].atom == DW_OP_fbreg) {
        if (fr == NULL || !fb_is_cfa) return -1;
        a = fr->cfa + (uint32_t)(int32_t)ops[0].number;
    } else {
        return -1;
    }
    *addr = a & 0xFFFFu;                              /* 16-bit SRAM data space  */
    return 0;
}

/* See elf_parser.h. */
int elf_var_addr(const ElfContext *ctx, uint32_t pc, const ElfFrameRegs *fr,
                 const char *name, uint32_t *addr, int *size, bool *is_signed)
{
    if (ctx == NULL || ctx->dwarf == NULL || name == NULL) return -1;
    Dwarf *dw = (Dwarf *)ctx->dwarf;

    Dwarf_Die cu;
    if (dwarf_addrdie(dw, (Dwarf_Addr)pc, &cu) == NULL) return -1;

    Dwarf_Die *scopes = NULL;
    int n = dwarf_getscopes(&cu, (Dwarf_Addr)pc, &scopes);
    if (n < 1 || scopes == NULL) { free(scopes); return -1; }

    bool      fb_is_cfa = cfi_frame_base_is_cfa(scopes, n);
    Dwarf_Die var;
    int       si = dwarf_getscopevar(scopes, n, name, 0, NULL, 0, 0, &var);
    free(scopes);                       /* `var` is an independent copy */
    if (si < 0) return -1;

    uint32_t a;
    if (eval_var_loc(&var, fr, fb_is_cfa, &a) != 0) return -1;
    if (addr != NULL) *addr = a;
    var_type_info(&var, size, is_signed);
    return 0;
}

/* type_off of a DIE's DW_AT_type (the global DIE offset, 0 if none). */
static uint64_t die_type_off(Dwarf_Die *die)
{
    Dwarf_Attribute ta;
    Dwarf_Die td;
    if (dwarf_attr_integrate(die, DW_AT_type, &ta) == NULL) return 0;
    if (dwarf_formref_die(&ta, &td) == NULL) return 0;
    return (uint64_t)dwarf_dieoffset(&td);
}

/* See elf_parser.h. */
int elf_var_enum(const ElfContext *ctx, uint32_t pc, const ElfFrameRegs *fr,
                 int scope, ElfVar *out, int max)
{
    if (ctx == NULL || ctx->dwarf == NULL || out == NULL || max <= 0) return -1;
    Dwarf *dw = (Dwarf *)ctx->dwarf;

    Dwarf_Die cu;
    if (dwarf_addrdie(dw, (Dwarf_Addr)pc, &cu) == NULL) return -1;

    int count = 0;

    if (scope == ELF_SCOPE_GLOBALS) {
        /* File-scope variables = direct DW_TAG_variable children of the CU. */
        Dwarf_Die child;
        if (dwarf_child(&cu, &child) != 0) return 0;
        do {
            if (dwarf_tag(&child) != DW_TAG_variable) continue;
            const char *nm = dwarf_diename(&child);
            if (nm == NULL) continue;
            uint32_t a;
            if (eval_var_loc(&child, NULL, false, &a) != 0) continue; /* DW_OP_addr only */
            if (count >= max) break;
            snprintf(out[count].name, sizeof out[count].name, "%s", nm);
            out[count].addr     = a;
            out[count].type_off = die_type_off(&child);
            count++;
        } while (dwarf_siblingof(&child, &child) == 0);
        return count;
    }

    /* LOCALS: params + locals of every lexical scope containing the PC. */
    Dwarf_Die *scopes = NULL;
    int n = dwarf_getscopes(&cu, (Dwarf_Addr)pc, &scopes);
    if (n < 1 || scopes == NULL) { free(scopes); return -1; }
    bool fb_is_cfa = cfi_frame_base_is_cfa(scopes, n);

    for (int i = 0; i < n && count < max; i++) {
        int tag = dwarf_tag(&scopes[i]);
        if (tag != DW_TAG_subprogram && tag != DW_TAG_lexical_block &&
            tag != DW_TAG_inlined_subroutine)
            continue;
        Dwarf_Die child;
        if (dwarf_child(&scopes[i], &child) != 0) continue;
        do {
            int ct = dwarf_tag(&child);
            if (ct != DW_TAG_variable && ct != DW_TAG_formal_parameter) continue;
            const char *nm = dwarf_diename(&child);
            if (nm == NULL) continue;
            uint32_t a;
            if (eval_var_loc(&child, fr, fb_is_cfa, &a) != 0) continue;
            if (count >= max) break;
            snprintf(out[count].name, sizeof out[count].name, "%s", nm);
            out[count].addr     = a;
            out[count].type_off = die_type_off(&child);
            count++;
        } while (dwarf_siblingof(&child, &child) == 0);
    }
    free(scopes);
    return count;
}

/* Read a little-endian unsigned integer of `size` (1..4) bytes via `read`. */
static int read_uint(ElfMemRead read, void *user, uint32_t addr, int size,
                     uint32_t *val)
{
    uint8_t b[4] = {0};
    if (size < 1 || size > 4) return -1;
    if (read(user, addr, b, size) != 0) return -1;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)b[i] << (8 * i);
    *val = v;
    return 0;
}

/* Render a peeled base/pointer/enum scalar at `addr`. */
static void render_scalar(Dwarf_Die *type, int tag, uint32_t addr,
                          ElfMemRead read, void *user, char *out, size_t cap)
{
    int bs = dwarf_bytesize(type);
    if (bs < 1 || bs > 4) bs = 2;

    if (tag == DW_TAG_pointer_type) {
        uint32_t v = 0;
        if (read_uint(read, user, addr, bs, &v) != 0) { snprintf(out, cap, "<?>"); return; }
        snprintf(out, cap, "0x%0*x", bs * 2, v);
        return;
    }

    Dwarf_Word enc = 0;
    Dwarf_Attribute ea;
    if (dwarf_attr(type, DW_AT_encoding, &ea) != NULL)
        (void)dwarf_formudata(&ea, &enc);

    if (tag == DW_TAG_enumeration_type) {
        uint32_t v = 0;
        if (read_uint(read, user, addr, bs, &v) != 0) { snprintf(out, cap, "<?>"); return; }
        /* Map to an enumerator name when one matches. */
        Dwarf_Die e;
        if (dwarf_child(type, &e) == 0) {
            do {
                if (dwarf_tag(&e) != DW_TAG_enumerator) continue;
                Dwarf_Attribute va; Dwarf_Word cv = 0;
                if (dwarf_attr(&e, DW_AT_const_value, &va) &&
                    dwarf_formudata(&va, &cv) == 0 && (uint32_t)cv == v) {
                    snprintf(out, cap, "%s (%u)", dwarf_diename(&e), v);
                    return;
                }
            } while (dwarf_siblingof(&e, &e) == 0);
        }
        snprintf(out, cap, "%u", v);
        return;
    }

    uint32_t raw = 0;
    if (read_uint(read, user, addr, bs, &raw) != 0) { snprintf(out, cap, "<?>"); return; }

    if (enc == DW_ATE_boolean) { snprintf(out, cap, "%s", raw ? "true" : "false"); return; }
    if (enc == DW_ATE_float && bs == 4) {
        float f; memcpy(&f, &raw, 4); snprintf(out, cap, "%g", (double)f); return;
    }
    if (enc == DW_ATE_signed || enc == DW_ATE_signed_char) {
        long s = (long)raw;
        if (bs < 4 && (raw & (1u << (bs * 8 - 1)))) s |= -(1L << (bs * 8));
        if (enc == DW_ATE_signed_char && s >= 32 && s < 127)
            snprintf(out, cap, "%ld '%c'", s, (char)s);
        else
            snprintf(out, cap, "%ld", s);
        return;
    }
    if (enc == DW_ATE_unsigned_char && raw >= 32 && raw < 127) {
        snprintf(out, cap, "%u '%c'", raw, (char)raw); return;
    }
    snprintf(out, cap, "%u", raw);                   /* unsigned / default */
}

/* Forward decl for bounded recursive aggregate rendering. */
static void render_value(const ElfContext *ctx, uint32_t addr, uint64_t type_off,
                         ElfMemRead read, void *user, int depth,
                         char *out, size_t cap, bool *expandable);

/* Array element type + element size + element count from an array DIE. */
static int array_info(const ElfContext *ctx, Dwarf_Die *arr,
                      uint64_t *elem_off, int *elem_size, int *count)
{
    Dwarf *dw = (Dwarf *)ctx->dwarf;
    *elem_off = die_type_off(arr);
    if (*elem_off == 0) return -1;
    Dwarf_Die et, etp;
    if (dwarf_offdie(dw, (Dwarf_Off)*elem_off, &et) == NULL) return -1;
    if (dwarf_peel_type(&et, &etp) != 0) etp = et;
    int es = dwarf_bytesize(&etp);
    *elem_size = (es >= 1) ? es : 1;
    *count = 0;
    Dwarf_Die sub;
    if (dwarf_child(arr, &sub) == 0) {
        do {
            if (dwarf_tag(&sub) != DW_TAG_subrange_type) continue;
            Dwarf_Attribute a; Dwarf_Word w = 0;
            if (dwarf_attr(&sub, DW_AT_count, &a) && dwarf_formudata(&a, &w) == 0)
                *count = (int)w;
            else if (dwarf_attr(&sub, DW_AT_upper_bound, &a) &&
                     dwarf_formudata(&a, &w) == 0)
                *count = (int)w + 1;
            break;
        } while (dwarf_siblingof(&sub, &sub) == 0);
    }
    return 0;
}

/* Member byte offset within its struct/union (DW_AT_data_member_location). */
static uint32_t member_offset(Dwarf_Die *member)
{
    Dwarf_Attribute a;
    if (dwarf_attr(member, DW_AT_data_member_location, &a) == NULL) return 0;
    Dwarf_Word w = 0;
    if (dwarf_formudata(&a, &w) == 0) return (uint32_t)w;
    Dwarf_Op *ops = NULL; size_t nops = 0;        /* DW_OP_plus_uconst form */
    if (dwarf_getlocation(&a, &ops, &nops) == 0 && nops == 1 &&
        ops[0].atom == DW_OP_plus_uconst)
        return (uint32_t)ops[0].number;
    return 0;
}

static void render_value(const ElfContext *ctx, uint32_t addr, uint64_t type_off,
                         ElfMemRead read, void *user, int depth,
                         char *out, size_t cap, bool *expandable)
{
    if (expandable) *expandable = false;
    if (cap == 0) return;
    out[0] = '\0';
    Dwarf *dw = (Dwarf *)ctx->dwarf;
    if (type_off == 0) { snprintf(out, cap, "<void>"); return; }

    Dwarf_Die td, tp;
    if (dwarf_offdie(dw, (Dwarf_Off)type_off, &td) == NULL) { snprintf(out, cap, "<?>"); return; }
    if (dwarf_peel_type(&td, &tp) != 0) tp = td;
    int tag = dwarf_tag(&tp);

    if (tag == DW_TAG_structure_type || tag == DW_TAG_union_type) {
        if (expandable) *expandable = true;
        if (depth <= 0) { snprintf(out, cap, "{...}"); return; }
        size_t off = 0; out[off++] = '{'; out[off] = '\0';
        Dwarf_Die m; int first = 1;
        if (dwarf_child(&tp, &m) == 0) {
            do {
                if (dwarf_tag(&m) != DW_TAG_member) continue;
                const char *mn = dwarf_diename(&m);
                uint32_t maddr = addr + member_offset(&m);
                char val[128];
                render_value(ctx, maddr, die_type_off(&m), read, user,
                             depth - 1, val, sizeof val, NULL);
                int w = snprintf(out + off, cap - off, "%s%s = %s",
                                 first ? "" : ", ", mn ? mn : "?", val);
                if (w < 0 || (size_t)w >= cap - off) break;
                off += (size_t)w; first = 0;
            } while (dwarf_siblingof(&m, &m) == 0);
        }
        if (off < cap - 1) { out[off++] = '}'; out[off] = '\0'; }
        return;
    }

    if (tag == DW_TAG_array_type) {
        if (expandable) *expandable = true;
        uint64_t eoff; int esz, n;
        if (array_info(ctx, &tp, &eoff, &esz, &n) != 0 || depth <= 0) {
            snprintf(out, cap, "{...}"); return;
        }
        size_t off = 0; out[off++] = '{'; out[off] = '\0';
        int shown = n < 16 ? n : 16;
        for (int i = 0; i < shown; i++) {
            char val[64];
            render_value(ctx, addr + (uint32_t)(i * esz), eoff, read, user,
                         depth - 1, val, sizeof val, NULL);
            int w = snprintf(out + off, cap - off, "%s%s", i ? ", " : "", val);
            if (w < 0 || (size_t)w >= cap - off) break;
            off += (size_t)w;
        }
        if (shown < n && off < cap - 6) off += (size_t)snprintf(out + off, cap - off, ", ...");
        if (off < cap - 1) { out[off++] = '}'; out[off] = '\0'; }
        return;
    }

    /* scalar / pointer / enum */
    render_scalar(&tp, tag, addr, read, user, out, cap);
}

/* See elf_parser.h. */
int elf_type_render(const ElfContext *ctx, uint32_t addr, uint64_t type_off,
                    ElfMemRead read, void *user,
                    char *out, size_t cap, bool *expandable)
{
    if (ctx == NULL || ctx->dwarf == NULL || read == NULL || out == NULL)
        return -1;
    render_value(ctx, addr, type_off, read, user, 3, out, cap, expandable);
    return 0;
}

/* See elf_parser.h. */
int elf_type_children(const ElfContext *ctx, uint32_t addr, uint64_t type_off,
                      ElfVar *out, int max)
{
    if (ctx == NULL || ctx->dwarf == NULL || out == NULL || max <= 0) return -1;
    Dwarf *dw = (Dwarf *)ctx->dwarf;
    if (type_off == 0) return -1;

    Dwarf_Die td, tp;
    if (dwarf_offdie(dw, (Dwarf_Off)type_off, &td) == NULL) return -1;
    if (dwarf_peel_type(&td, &tp) != 0) tp = td;
    int tag = dwarf_tag(&tp);
    int count = 0;

    if (tag == DW_TAG_structure_type || tag == DW_TAG_union_type) {
        Dwarf_Die m;
        if (dwarf_child(&tp, &m) != 0) return 0;
        do {
            if (dwarf_tag(&m) != DW_TAG_member) continue;
            const char *mn = dwarf_diename(&m);
            if (count >= max) break;
            snprintf(out[count].name, sizeof out[count].name, "%s", mn ? mn : "?");
            out[count].addr     = addr + member_offset(&m);
            out[count].type_off = die_type_off(&m);
            count++;
        } while (dwarf_siblingof(&m, &m) == 0);
        return count;
    }

    if (tag == DW_TAG_array_type) {
        uint64_t eoff; int esz, n;
        if (array_info(ctx, &tp, &eoff, &esz, &n) != 0) return -1;
        for (int i = 0; i < n && count < max; i++) {
            snprintf(out[count].name, sizeof out[count].name, "[%d]", i);
            out[count].addr     = addr + (uint32_t)(i * esz);
            out[count].type_off = eoff;
            count++;
        }
        return count;
    }

    return -1;                                       /* not an aggregate */
}

/* See elf_parser.h. */
int elf_var_find(const ElfContext *ctx, uint32_t pc, const ElfFrameRegs *fr,
                 const char *name, uint32_t *addr, uint64_t *type_off)
{
    if (ctx == NULL || ctx->dwarf == NULL || name == NULL) return -1;
    Dwarf *dw = (Dwarf *)ctx->dwarf;

    Dwarf_Die cu;
    if (dwarf_addrdie(dw, (Dwarf_Addr)pc, &cu) == NULL) return -1;

    Dwarf_Die *scopes = NULL;
    int n = dwarf_getscopes(&cu, (Dwarf_Addr)pc, &scopes);
    if (n < 1 || scopes == NULL) { free(scopes); return -1; }
    bool fb_is_cfa = cfi_frame_base_is_cfa(scopes, n);
    Dwarf_Die var;
    int si = dwarf_getscopevar(scopes, n, name, 0, NULL, 0, 0, &var);
    free(scopes);
    if (si < 0) return -1;

    uint32_t a;
    if (eval_var_loc(&var, fr, fb_is_cfa, &a) != 0) return -1;
    if (addr != NULL)     *addr = a;
    if (type_off != NULL) *type_off = die_type_off(&var);
    return 0;
}

/* See elf_parser.h. */
int elf_type_size(const ElfContext *ctx, uint64_t type_off)
{
    if (ctx == NULL || ctx->dwarf == NULL || type_off == 0) return 2;
    Dwarf_Die td, tp;
    if (dwarf_offdie((Dwarf *)ctx->dwarf, (Dwarf_Off)type_off, &td) == NULL) return 2;
    if (dwarf_peel_type(&td, &tp) != 0) tp = td;
    int bs = dwarf_bytesize(&tp);
    return (bs >= 1 && bs <= 4) ? bs : 2;
}

/* See elf_parser.h. */
int elf_addr_to_func(const ElfContext *ctx, uint32_t pc, char *name, size_t cap)
{
    if (ctx == NULL || ctx->dwarf == NULL || name == NULL || cap == 0)
        return -1;

    Dwarf_Die cu;
    if (dwarf_addrdie((Dwarf *)ctx->dwarf, (Dwarf_Addr)pc, &cu) == NULL)
        return -1;

    Dwarf_Die *scopes = NULL;
    int n = dwarf_getscopes(&cu, (Dwarf_Addr)pc, &scopes);
    if (n < 1 || scopes == NULL) { free(scopes); return -1; }

    const char *fn = NULL;
    for (int i = 0; i < n; i++) {
        if (dwarf_tag(&scopes[i]) == DW_TAG_subprogram) {
            fn = dwarf_diename(&scopes[i]);
            break;
        }
    }
    free(scopes);
    if (fn == NULL) return -1;

    strncpy(name, fn, cap - 1);
    name[cap - 1] = '\0';
    return 0;
}
