/* src/debug_bp.c — protocol-agnostic breakpoint core (see debug_bp.h).
 *
 * Lifted verbatim (behaviour-preserving) from the static breakpoint policy
 * that lived in src/gdb_rsp.c through Phase 17, parameterised on the
 * caller's breakpoint state so both the RSP and DAP front-ends share one
 * implementation (HLR-073).  No protocol formatting here.
 */
#include "debug_bp.h"
#include "updi.h"   /* UPDI_FLASH_BASE, GDB_AVR_*, updi_* OCD/NVM primitives */

const uint8_t SW_BP_BREAK_BYTES[2] = { 0x98u, 0x95u };

/* ── CPU register-file snapshot / restore around the NVMPROG reset ─────────── */

int bp_snapshot_cpu(int updi_fd, uint8_t gpr[32],
                    uint8_t *sreg, uint16_t *sp, uint32_t *pc)
{
    for (int i = 0; i < 32; i++) {
        if (updi_ocd_read_gpr(updi_fd, (uint8_t)i, &gpr[i]) < 0) return -1;
    }
    if (updi_ocd_read_sreg(updi_fd, sreg) < 0) return -1;
    if (updi_ocd_read_sp  (updi_fd, sp)   < 0) return -1;
    if (updi_ocd_read_pc  (updi_fd, pc)   < 0) return -1;
    return 0;
}

int bp_restore_cpu(int updi_fd, const uint8_t gpr[32],
                   uint8_t sreg, uint16_t sp, uint32_t pc)
{
    for (int i = 0; i < 32; i++) {
        if (updi_ocd_write_gpr(updi_fd, (uint8_t)i, gpr[i]) < 0) return -1;
    }
    if (updi_ocd_write_sreg(updi_fd, sreg) < 0) return -1;
    if (updi_ocd_write_sp  (updi_fd, sp)   < 0) return -1;
    if (updi_ocd_write_pc  (updi_fd, pc)   < 0) return -1;
    if (updi_ocd_stabilize_pc_after_write(updi_fd) < 0) return -1;
    return 0;
}

/* ── SW-breakpoint FLASH patch (preserves CPU + peripheral state) ──────────── */

int bp_sw_patch_flash(int updi_fd, const uint32_t hw_bp_addr[2],
                      bool *pc_dirty, uint32_t byte_addr,
                      const uint8_t bytes[2], uint8_t orig_out[2])
{
    uint8_t  gpr[32], sreg;
    uint16_t sp;
    uint32_t pc;
    /* Phase 20: peripheral I/O snapshot preserved across the NVMPROG system
     * reset so any SLEEP wake source (timer/USART/pin/...) survives.  No-op
     * (and `periph` left untouched) when debug-in-sleep is disabled (--sleep). */
    static uint8_t periph[UPDI_PERIPH_LEN];

    if (bp_snapshot_cpu(updi_fd, gpr, &sreg, &sp, &pc) < 0) return -1;
    if (updi_save_peripherals(updi_fd, periph) < 0) return -1;

    if (orig_out != NULL) {
        if (updi_mem_read(updi_fd, UPDI_FLASH_BASE + byte_addr,
                          orig_out, 2) < 0)
            return -1;
    }

    if (updi_nvm_flash_patch(updi_fd, UPDI_FLASH_BASE + byte_addr,
                             bytes, 2) < 0)
        return -1;

    if (updi_enter_debug(updi_fd) < 0) return -1;

    /* The NVMPROG reset cleared the OCD comparators — re-arm any live ones. */
    for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++) {
        if (hw_bp_addr[i] != HW_BP_SLOT_EMPTY) {
            uint32_t flash_byte = hw_bp_addr[i] & GDB_AVR_ADDR_MASK;
            if (updi_ocd_set_hw_bp(updi_fd, i, flash_byte) < 0)
                return -1;
        }
    }

    /* Restore the peripheral configuration the reset wiped, before re-arming
     * the CPU state (which writes OCD.PC last for the pc_dirty handling). */
    if (updi_restore_peripherals(updi_fd, periph) < 0) return -1;

    if (bp_restore_cpu(updi_fd, gpr, sreg, sp, pc) < 0) return -1;

    /* The restore above re-wrote OCD.PC after the NVMPROG system-reset.
     * A fresh OCD.PC write makes the silicon skip the instruction at PC on
     * the next step/run; mark the PC dirty so the next resume executes that
     * instruction via injection (bp_consume_pc_skip) rather than skipping. */
    *pc_dirty = true;
    return 0;
}

/* ── Install / remove ─────────────────────────────────────────────────────── */

/* Install one SW breakpoint into the shadow + FLASH.  Returns BpStatus. */
static int bp_sw_install(int updi_fd, const uint32_t hw_bp_addr[2],
                         RspSwBp sw_bp[], bool *pc_dirty, uint32_t addr)
{
    if (addr & GDB_AVR_DATA_FLAG) return BP_ERR_DATASP;
    for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++)
        if (sw_bp[i].in_use && sw_bp[i].addr == addr) return BP_OK;
    int slot_i = -1;
    for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++)
        if (!sw_bp[i].in_use) { slot_i = i; break; }
    if (slot_i < 0) return BP_ERR_SLOT;
    uint8_t orig[2];
    if (bp_sw_patch_flash(updi_fd, hw_bp_addr, pc_dirty,
                          addr & GDB_AVR_ADDR_MASK, SW_BP_BREAK_BYTES, orig) < 0)
        return BP_ERR_IO;
    sw_bp[slot_i].in_use  = true;
    sw_bp[slot_i].addr    = addr;
    sw_bp[slot_i].orig[0] = orig[0];
    sw_bp[slot_i].orig[1] = orig[1];
    return BP_OK;
}

int bp_insert(int updi_fd, uint32_t hw_bp_addr[2], bool hw_bp_pinned[2],
              RspSwBp sw_bp[], int bp_mode, bool *pc_dirty,
              char kind, uint32_t addr)
{
    /* Decide whether this breakpoint wants a HW comparator and, if so,
     * whether it pins the slot (HLR-054/055):
     *   - Z1 (hbreak)        → HW, pinned.
     *   - Z0 `hw-only`       → HW, pinned (explicit user HW choice).
     *   - Z0 `sw`            → SW.
     *   - Z0 `auto`          → HW (evictable) when a slot is free, else SW. */
    bool want_hw, pinned;
    if (kind == '1') {
        want_hw = true;  pinned = true;
    } else if (bp_mode == RSP_BP_MODE_HW_ONLY) {
        want_hw = true;  pinned = true;
    } else if (bp_mode == RSP_BP_MODE_SW) {
        want_hw = false; pinned = false;
    } else { /* RSP_BP_MODE_AUTO */
        if (addr & GDB_AVR_DATA_FLAG) return BP_ERR_DATASP;
        for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++)
            if (sw_bp[i].in_use && sw_bp[i].addr == addr) return BP_OK;
        for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++)
            if (hw_bp_addr[i] == addr) return BP_OK;
        bool hw_free = false;
        for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++)
            if (hw_bp_addr[i] == HW_BP_SLOT_EMPTY) { hw_free = true; break; }
        want_hw = hw_free;  pinned = false;
    }

    if (!want_hw)
        return bp_sw_install(updi_fd, hw_bp_addr, sw_bp, pc_dirty, addr);

    /* HW comparator path. */
    for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++)
        if (hw_bp_addr[i] == addr) return BP_OK;            /* idempotent */
    int slot_i = -1;
    for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++)
        if (hw_bp_addr[i] == HW_BP_SLOT_EMPTY) { slot_i = i; break; }
    if (slot_i < 0) {
        /* No free comparator.  A pinned request (Z1 / hw-only) evicts an
         * evictable auto-Z0 to a SW breakpoint so an explicit hardware
         * breakpoint always wins the slot. */
        if (!pinned) return BP_ERR_SLOT;
        int ev = -1;
        for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++)
            if (!hw_bp_pinned[i]) { ev = i; break; }
        if (ev < 0) return BP_ERR_SLOT;       /* both slots pinned */
        uint32_t ev_addr = hw_bp_addr[ev];
        if (updi_ocd_clear_hw_bp(updi_fd, ev) < 0) return BP_ERR_IO;
        hw_bp_addr[ev]   = HW_BP_SLOT_EMPTY;
        hw_bp_pinned[ev] = false;
        int st = bp_sw_install(updi_fd, hw_bp_addr, sw_bp, pc_dirty, ev_addr);
        if (st != BP_OK) return st;
        slot_i = ev;
    }
    if (updi_ocd_set_hw_bp(updi_fd, slot_i, addr & GDB_AVR_ADDR_MASK) < 0)
        return BP_ERR_IO;
    hw_bp_addr[slot_i]   = addr;
    hw_bp_pinned[slot_i] = pinned;
    return BP_OK;
}

int bp_remove(int updi_fd, uint32_t hw_bp_addr[2], bool hw_bp_pinned[2],
              RspSwBp sw_bp[], int bp_mode, bool *pc_dirty,
              char kind, uint32_t addr)
{
    /* A z0 in sw/auto mode drains the SW shadow first; on no match (or a
     * mode toggle / auto-HW breakpoint) it falls through to the HW scan. */
    if (kind == '0' && (bp_mode == RSP_BP_MODE_SW ||
                        bp_mode == RSP_BP_MODE_AUTO)) {
        for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
            if (sw_bp[i].in_use && sw_bp[i].addr == addr) {
                uint32_t byte_addr = addr & GDB_AVR_ADDR_MASK;
                if (bp_sw_patch_flash(updi_fd, hw_bp_addr, pc_dirty, byte_addr,
                                      sw_bp[i].orig, NULL) < 0)
                    return BP_ERR_IO;
                sw_bp[i].in_use  = false;
                sw_bp[i].addr    = 0;
                sw_bp[i].orig[0] = 0;
                sw_bp[i].orig[1] = 0;
                return BP_OK;
            }
        }
    }

    for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++) {
        if (hw_bp_addr[i] == addr) {
            if (updi_ocd_clear_hw_bp(updi_fd, i) < 0) return BP_ERR_IO;
            hw_bp_addr[i]   = HW_BP_SLOT_EMPTY;
            hw_bp_pinned[i] = false;
            return BP_OK;
        }
    }
    return BP_OK;   /* no record — a GDB resync is non-fatal */
}

/* ── Stop-cause classification + PC-skip injection ────────────────────────── */

int bp_classify_stop(int updi_fd, const RspSwBp sw_bp[],
                     const uint32_t hw_bp_addr[2], int hint)
{
    uint32_t pc = 0;
    if (updi_ocd_read_pc(updi_fd, &pc) < 0) return hint;
    for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
        if (sw_bp[i].in_use && sw_bp[i].addr == pc) return SC_SWBREAK;
    }
    for (int i = 0; i < RSP_HW_BP_USER_SLOTS; i++) {
        if (hw_bp_addr[i] != HW_BP_SLOT_EMPTY && hw_bp_addr[i] == pc)
            return SC_HWBREAK;
    }
    return hint;
}

int bp_consume_pc_skip(int updi_fd, RspSwBp sw_bp[], bool *pc_dirty)
{
    if (!*pc_dirty) return 0;
    *pc_dirty = false;

    uint32_t pc = 0;
    if (updi_ocd_read_pc(updi_fd, &pc) < 0) return -1;

    uint8_t op[4] = {0};
    if (updi_mem_read(updi_fd, pc | UPDI_FLASH_BASE, op, 4) < 0) return -1;
    uint16_t w0 = (uint16_t)op[0] | ((uint16_t)op[1] << 8);

    /* Leave a direct 32-bit CALL/JMP to the caller's CoF emulation. */
    if ((w0 & 0xFE0Eu) == 0x940Eu || (w0 & 0xFE0Eu) == 0x940Cu)
        return 0;

    /* If a BREAK is patched at PC (an active SW breakpoint we are resuming
     * over), inject the saved original opcode word instead. */
    uint16_t exec_w0 = w0;
    if (w0 == ((uint16_t)SW_BP_BREAK_BYTES[0]
               | ((uint16_t)SW_BP_BREAK_BYTES[1] << 8))) {
        for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
            if (sw_bp[i].in_use && sw_bp[i].addr == pc) {
                exec_w0 = (uint16_t)sw_bp[i].orig[0]
                        | ((uint16_t)sw_bp[i].orig[1] << 8);
                break;
            }
        }
    }

    if (updi_ocd_step_inject_word0(updi_fd, exec_w0) < 0) return -1;
    return 1;
}

int bp_step_over(int updi_fd, RspSwBp sw_bp[], bool *pc_dirty)
{
    /* A pending fresh-PC-write / patched-BREAK is executed via injection;
     * consume_pc_skip() does exactly one instruction when it returns 1 — that
     * IS this step.  A direct 32-bit CALL/JMP returns 0 and falls through to
     * the change-of-flow path below. */
    int cs = bp_consume_pc_skip(updi_fd, sw_bp, pc_dirty);
    if (cs < 0) return -1;
    if (cs == 1) return 0;

    /* Detect a 32-bit instruction (AVR UPDI hardware-stepper errata). */
    uint32_t pc_byte = 0;
    if (updi_ocd_read_pc(updi_fd, &pc_byte) < 0) return -1;

    uint8_t opcode[4] = {0};
    if (updi_mem_read(updi_fd, pc_byte | UPDI_FLASH_BASE, opcode, 4) < 0) return -1;

    uint16_t w0 = (uint16_t)opcode[0] | ((uint16_t)opcode[1] << 8);
    uint16_t w1 = (uint16_t)opcode[2] | ((uint16_t)opcode[3] << 8);

    bool     is_32bit      = false;
    bool     is_call_32bit = false;     /* direct 32-bit CALL — needs return push */
    bool     is_jmp_32bit  = false;     /* direct 32-bit JMP  — no return push    */
    uint32_t target_pc     = pc_byte + 4;

    if ((w0 & 0xFE0F) == 0x9000 || (w0 & 0xFE0F) == 0x9200) {
        is_32bit = true;                /* LDS / STS (32-bit non-CoF) */
    } else if ((w0 & 0xFE0E) == 0x940E) {
        uint32_t k = (uint32_t)w1
                   | (((uint32_t)w0 & 0x0001u) << 16)
                   | ((((uint32_t)w0 & 0x01F0u) >> 4) << 17);
        target_pc = k * 2u; is_32bit = true; is_call_32bit = true;
    } else if ((w0 & 0xFE0E) == 0x940C) {
        uint32_t k = (uint32_t)w1
                   | (((uint32_t)w0 & 0x0001u) << 16)
                   | ((((uint32_t)w0 & 0x01F0u) >> 4) << 17);
        target_pc = k * 2u; is_32bit = true; is_jmp_32bit = true;
    }
    bool halt_on_jump = is_call_32bit || is_jmp_32bit;

    if (is_32bit) {
        /* HLR-062 (issue #40): direct 32-bit CoF (CALL/JMP) is missed by the
         * OCD comparator / OCD_CTRL1_JMP on the first change-of-flow after a
         * RUN, so emulate it over OCD primitives.  emulate_cof writes a fresh
         * OCD.PC at the target → mark pc_dirty so the next resume injects the
         * skipped instruction (Appendix B.8, Group-G G19).  Plain HW-BP@PC+4
         * (updi_step_32bit) remains correct for the non-CoF LDS/STS case. */
        if (is_call_32bit) {
            if (updi_ocd_emulate_cof_32bit(updi_fd, pc_byte + 4u, target_pc) < 0)
                return -1;
            *pc_dirty = true;
        } else if (is_jmp_32bit) {
            if (updi_ocd_emulate_cof_32bit(updi_fd, 0u, target_pc) < 0)
                return -1;
            *pc_dirty = true;
        } else {
            if (updi_step_32bit(updi_fd, RSP_HW_BP_STEP_SLOT, target_pc,
                                halt_on_jump) < 0)
                return -1;
        }
    } else {
        if (updi_step(updi_fd) < 0) return -1;
    }
    return 0;
}

void bp_clear_all_sw(RspSwBp sw_bp[])
{
    for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
        sw_bp[i].in_use  = false;
        sw_bp[i].addr    = 0;
        sw_bp[i].orig[0] = 0;
        sw_bp[i].orig[1] = 0;
    }
}
