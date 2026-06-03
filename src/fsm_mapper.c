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
 *   fsmStateMachine_t       (in SRAM):
 *     +9  currState (fn *)           2 bytes   current state function ptr
 *
 *   currStateMachine        (file-static SRAM pointer, 2 bytes)
 *     The currently-running fsmStateMachine_t (set on each dispatch).
 *
 * Address spaces (all addresses below are GDB-AVR ELF form — bit 23
 * SET = data space, bit 23 CLEAR = flash):
 *   • idx->fsm_table_addr is a 16-bit data-space VMA inside the
 *     AVR-Dx mapped-flash window (0x8000..0xFFFF).  We route reads
 *     through UPDI's flash mirror via flash_to_updi() so the chip's
 *     FLMAP hardware resolves the physical page transparently.
 *   • idx->current_fsm_addr is a GDB-AVR data-space address; we strip
 *     the data flag with sram_to_updi() before passing it to UPDI.
 *   • Per-entry stateMachine pointers and currStateMachine are bare
 *     16-bit AVR data-space byte addresses (already in UPDI form,
 *     no flag bit set).  Per-entry name pointers are 16-bit C
 *     pointers; on AVR-Dx they land in the mapped-flash window
 *     (>= 0x8000) and we read them via flash_to_updi() likewise.
 */
#include <stdio.h>
#include <string.h>

#include "fsm_mapper.h"
#include "updi.h"   /* UPDI_FLASH_BASE, sram_to_updi, flash_to_updi */

extern int updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len);

#define FSM_DESCR_SIZE        9U   /* sizeof(fsmStateMachineDescr_t)        */
#define FSM_CURRSTATE_OFFSET  9U   /* offset of currState in fsmStateMachine_t */
#define REG_BUF_HEX_LEN      78U   /* 32+1+1+1+4 bytes × 2 hex chars         */

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
        if (b == 0)
            break;
        dst[off++] = (char)b;
    }
    dst[off] = '\0';
    return 0;
}

/* Encode one byte as two lower-case hex chars at *p, advance *p by 2. */
static void hex_byte(char **p, uint8_t b)
{
    static const char H[] = "0123456789abcdef";
    *(*p)++ = H[(b >> 4) & 0xFu];
    *(*p)++ = H[b & 0xFu];
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

        /* Read currState (function pointer) from SRAM at sm_ptr + 9.
         * sm_ptr is a raw 16-bit AVR data-space byte address (already
         * in UPDI form — no flag bit set). */
        uint8_t cs[2];
        if (updi_mem_read(updi_fd,
                          (uint32_t)sm_ptr + FSM_CURRSTATE_OFFSET,
                          cs, sizeof(cs)) < 0) {
            fprintf(stderr,
                    "fsm: failed to read currState at 0x%04x\n",
                    (unsigned)(sm_ptr + FSM_CURRSTATE_OFFSET));
            return -1;
        }
        t->state_fn = (uint32_t)read_le16(cs);

        /* Read the descriptor name (NUL-terminated, in FLASH).
         * name_ptr is a 16-bit AVR C pointer; on AVR-Dx parts string
         * literals live in the mapped-flash window (>= 0x8000) and
         * the chip resolves FLMAP via UPDI's flash mirror. */
        if (name_ptr != 0) {
            if (read_flash_string(updi_fd, (uint32_t)name_ptr,
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

/* ── fsm_get_registers ────────────────────────────────────────────────── *
 * Build a 78-char hex-encoded register payload (plus NUL) for `thread_id`.
 *
 * Layout (positions in the hex buffer):
 *   [ 0..63]  R0..R31           (32 bytes × 2 hex)  — always zero in stub
 *   [64..65]  SREG              ( 1 byte  × 2 hex)
 *   [66..67]  SPL               ( 1 byte  × 2 hex)
 *   [68..69]  SPH               ( 1 byte  × 2 hex)
 *   [70..77]  PC (4-byte LE)    (state_fn, zero-extended)
 *   [78]      '\0'
 *
 * For the active thread, SREG/SPL/SPH are read live over UPDI.
 * For non-active threads we issue ZERO UPDI reads (LLR-FSM-06): the
 * stack frame in SRAM doesn't carry an inspectable CPU state, so we
 * report zeros for those bytes and the FSM's currState as the PC.
 */
int fsm_get_registers(const FsmContext *ctx, int thread_id, char *reg_buf)
{
    if (ctx == NULL || reg_buf == NULL || !ctx->valid)
        return -1;

    const FsmThread *t = NULL;
    for (int i = 0; i < ctx->thread_count; i++) {
        if (ctx->threads[i].gdb_id == thread_id) {
            t = &ctx->threads[i];
            break;
        }
    }
    if (t == NULL)
        return -1;

    /* Start with a fully-zeroed hex buffer (covers R0..R31 and any
     * field we don't subsequently overwrite). */
    memset(reg_buf, '0', REG_BUF_HEX_LEN);
    reg_buf[REG_BUF_HEX_LEN] = '\0';

    uint8_t sreg = 0, spl = 0, sph = 0;
    if (t->is_active) {
        /* Live read of CPU state register + stack pointer (AVR128DA
         * I/O space: SREG @0x003F, SPL @0x003D, SPH @0x003E). */
        uint8_t io[3];
        if (updi_mem_read(0 /* fd unused by mock */, 0x3DU, io, 3) < 0)
            return -1;
        spl  = io[0];
        sph  = io[1];
        sreg = io[2];
    }

    /* Emit SREG, SPL, SPH (positions 64..69). */
    char *p = reg_buf + 64;
    hex_byte(&p, sreg);  /* 64..65 */
    hex_byte(&p, spl);   /* 66..67 */
    hex_byte(&p, sph);   /* 68..69 */

    /* Emit PC: 4 bytes LE of state_fn (positions 70..77). */
    uint32_t pc = t->state_fn;
    hex_byte(&p, (uint8_t)(pc      ));
    hex_byte(&p, (uint8_t)(pc >>  8));
    hex_byte(&p, (uint8_t)(pc >> 16));
    hex_byte(&p, (uint8_t)(pc >> 24));

    return 0;
}
