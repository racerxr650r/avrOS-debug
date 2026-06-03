/* src/fsm_mapper.c — avrOS FSM-table → GDB-thread mapper (Phase 3)
 *
 * Walks the FSM_TABLE in FLASH via UPDI, follows each entry's
 * fsmStateMachine_t pointer into SRAM, and builds the per-thread
 * register payload exposed via the gdb_rsp `g`/`T` packets.
 *
 * Real avrOS descriptors (see sys/fsm.h; per-entry stride matches
 * the 1-byte-packed AVR layout — no padding):
 *
 *   fsmStateMachineDescr_t  (9 bytes, in FLASH / FSM_TABLE):
 *     +0  name (char *)              2 bytes   FLASH pointer
 *     +2  stateMachine (...*)        2 bytes   SRAM pointer (NULL ⇒ skip)
 *     +4  handler (fn *)             2 bytes
 *     +6  priority (uint8_t)         1 byte
 *     +7  instance (uint16_t)        2 bytes
 *
 *   fsmStateMachine_t       (in SRAM, sizeof 21):
 *     +0  currStateName (char *)     2 bytes   FLASH ptr to current
 *                                              state's name string
 *                                              (NULL until first dispatch)
 *
 *   currStateMachine        (file-static SRAM pointer, 2 bytes)
 *     The currently-running fsmStateMachine_t (set on each dispatch).
 *
 * Address spaces (all addresses below are GDB-AVR ELF form — bit 23
 * SET = data space, bit 23 CLEAR = flash):
 *   • idx->fsm_table_addr is the physical FLASH byte address (LMA) of
 *     FSM_TABLE — elf_parser already translated the mapped-flash VMA to
 *     its LMA via the PT_LOAD p_paddr basis.  We read it through UPDI's
 *     flash mirror with flash_to_updi().  (UPDI flash reads are LINEAR:
 *     the mapped-flash window's FLMAP is NOT applied, which is exactly
 *     why the LMA — not the 0x8000-window VMA — must be used.)
 *   • idx->current_fsm_addr is a GDB-AVR data-space address; we strip
 *     the data flag with sram_to_updi() before passing it to UPDI.
 *   • Per-entry stateMachine pointers and currStateMachine are bare
 *     16-bit AVR data-space (SRAM) byte addresses, read directly.
 *   • Per-entry name pointers are data-space mapped-flash VMAs
 *     (>= 0x8000); we add idx->flash_lma_off to obtain the physical
 *     FLASH byte (LMA) before routing through flash_to_updi().
 */
#include <stdio.h>
#include <string.h>

#include "fsm_mapper.h"
#include "updi.h"   /* UPDI_FLASH_BASE, sram_to_updi, flash_to_updi */

extern int updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len);

#define FSM_DESCR_SIZE            9U   /* sizeof(fsmStateMachineDescr_t)    */
#define FSM_CURRSTATENAME_OFFSET  0U   /* offset of currStateName (char *)
                                        * in fsmStateMachine_t — the first
                                        * member; a FLASH string pointer to
                                        * the FSM's current state name.     */

/* AVR data-space pointers are 16-bit; zero-extend into uint32_t. */
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Read up to `max-1` bytes from FLASH starting at GDB-AVR byte address
 * `byte_addr`, stop on first NUL or after `max-1` bytes, NUL-terminate
 * `dst`.  Routes through UPDI's flash mirror (flash_to_updi); the chip
 * resolves any mapped-flash window pointer (0x8000..0xFFFF) via FLMAP
 * in hardware.  Returns 0 on success, -1 on UPDI failure. */
static int read_flash_string(int updi_fd, uint32_t byte_addr,
                             char *dst, size_t max)
{
    if (max == 0)
        return 0;

    uint32_t base_addr = flash_to_updi(byte_addr);

    size_t off = 0;
    while (off + 1 < max) {
        uint8_t b;
        if (updi_mem_read(updi_fd, base_addr + (uint32_t)off, &b, 1) < 0)
            return -1;
        /* Stop at NUL or the first non-printable byte: a stale/garbage
         * pointer (e.g. a NULL currStateName that resolved through the
         * flash mirror) then yields an empty string rather than binary
         * junk in the introspection output. */
        if (b == 0 || b < 0x20u || b > 0x7Eu)
            break;
        dst[off++] = (char)b;
    }
    dst[off] = '\0';
    return 0;
}

/* ── fsm_build_thread_list ─────────────────────────────────────────────
 * Walk the FSM_TABLE (idx->fsm_table_count × 9-byte entries) and produce
 * one FsmThread per entry whose stateMachine pointer is non-NULL.
 * Reads currStateMachine to mark the active thread.
 * Returns the number of threads (>=0) or -1 on UPDI failure.
 */
int fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int updi_fd)
{
    if (ctx == NULL || idx == NULL)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->active_id = 0; /* 0 = no active match */

    /* 1) Read currStateMachine (2-byte SRAM ptr) so we can mark the
     *    active entry as we walk the table. */
    uint16_t current_sm = 0;
    if (idx->current_fsm_addr != 0U) {
        uint8_t buf[2];
        if (updi_mem_read(updi_fd, sram_to_updi(idx->current_fsm_addr), buf, sizeof(buf)) < 0) {
            fprintf(stderr,
                    "fsm: failed to read currStateMachine at 0x%08x\n",
                    (unsigned)idx->current_fsm_addr);
            return -1;
        }
        current_sm = read_le16(buf);
    }

    /* 2) Walk the descriptor table.  Cap at FSM_MAX_THREADS but keep
     *    scanning to log overflow. */
    int produced = 0;
    int overflow = 0;
    const uint8_t count = idx->fsm_table_count;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t descr[FSM_DESCR_SIZE];
        uint32_t descr_addr = idx->fsm_table_addr
                            + (uint32_t)i * FSM_DESCR_SIZE;
        if (updi_mem_read(updi_fd, flash_to_updi(descr_addr),
                          descr, sizeof(descr)) < 0) {
            fprintf(stderr,
                    "fsm: failed to read FSM_TABLE[%u] at 0x%08x\n",
                    (unsigned)i, (unsigned)descr_addr);
            return -1;
        }

        uint16_t name_ptr = read_le16(&descr[0]);
        uint16_t sm_ptr   = read_le16(&descr[2]);

        /* Skip initializer / NULL-stateMachine entries. */
        if (sm_ptr == 0)
            continue;

        if (produced >= FSM_MAX_THREADS) {
            overflow++;
            continue;
        }

        FsmThread *t = &ctx->threads[produced];
        t->gdb_id   = FSM_FIRST_PSEUDO_THREAD_ID + produced;
        t->is_active = (sm_ptr == current_sm);

        /* Read currStateName (a FLASH string pointer) at offset 0 of the
         * fsmStateMachine_t, then resolve it to the FSM's current state
         * name.  sm_ptr is a raw 16-bit AVR data-space (SRAM) byte address
         * (already in UPDI form — no flag bit set).  A NULL pointer means
         * the FSM has not dispatched yet → empty state name. */
        uint8_t csn[2];
        if (updi_mem_read(updi_fd,
                          (uint32_t)sm_ptr + FSM_CURRSTATENAME_OFFSET,
                          csn, sizeof(csn)) < 0) {
            fprintf(stderr,
                    "fsm: failed to read currStateName at 0x%04x\n",
                    (unsigned)(sm_ptr + FSM_CURRSTATENAME_OFFSET));
            return -1;
        }
        uint16_t state_name_ptr = read_le16(csn);
        if (state_name_ptr != 0) {
            if (read_flash_string(updi_fd,
                                  (uint32_t)state_name_ptr + idx->flash_lma_off,
                                  t->state_name, sizeof(t->state_name)) < 0) {
                fprintf(stderr,
                        "fsm: failed to read currStateName string at 0x%04x\n",
                        (unsigned)state_name_ptr);
                return -1;
            }
        } else {
            t->state_name[0] = '\0';
        }

        /* Read the descriptor name (NUL-terminated, in FLASH).
         * name_ptr is a 16-bit AVR C pointer; on AVR-Dx parts string
         * literals live in the mapped-flash window (>= 0x8000) and
         * the chip resolves FLMAP via UPDI's flash mirror. */
        if (name_ptr != 0) {
            /* name_ptr is a data-space mapped-flash VMA; translate to the
             * physical FLASH byte (LMA) the same way the table address was
             * (idx->flash_lma_off) before routing through the UPDI flash
             * mirror — otherwise the read hits the wrong (FLMAP-unmapped)
             * linear flash offset and returns 0xFF garbage. */
            if (read_flash_string(updi_fd,
                                  (uint32_t)name_ptr + idx->flash_lma_off,
                                  t->name, sizeof(t->name)) < 0) {
                fprintf(stderr,
                        "fsm: failed to read name at 0x%04x\n",
                        (unsigned)name_ptr);
                return -1;
            }
        } else {
            t->name[0] = '\0';
        }

        if (t->is_active)
            ctx->active_id = t->gdb_id;

        produced++;
    }

    if (overflow > 0) {
        fprintf(stderr,
                "fsm: FSM_TABLE has %d entries beyond cap %d (ignored)\n",
                overflow, FSM_MAX_THREADS);
    }

    ctx->thread_count = produced;
    ctx->valid        = true;
    return produced;
}

/* ── fsm_invalidate ───────────────────────────────────────────────────── */
void fsm_invalidate(FsmContext *ctx)
{
    if (ctx != NULL) {
        memset(ctx->threads, 0, sizeof(ctx->threads));
        ctx->thread_count = 0;
        ctx->active_id = 0;
        ctx->valid = false;
    }
}

/* ── fsm_get_active_thread ────────────────────────────────────────────── */
int fsm_get_active_thread(const FsmContext *ctx)
{
    if (ctx == NULL || !ctx->valid)
        return 0;
    return ctx->active_id;
}

