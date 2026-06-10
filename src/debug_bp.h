/* src/debug_bp.h — protocol-agnostic breakpoint core (RSP + DAP).
 *
 * The HW-comparator arbiter and the software-breakpoint (FLASH `BREAK`)
 * model used to live as static policy inside src/gdb_rsp.c.  Phase 18 lifts
 * them into this shared core so the DAP front-end (src/dap.c) installs
 * breakpoints through exactly the same arbiter/SW-BP logic the proven
 * GDB-RSP path uses (HLR-073, the debug-core seam).
 *
 * The functions take the caller's breakpoint state (the HW-comparator
 * shadow `hw_bp_addr[2]`, the SW-BP shadow table `sw_bp[]`, the `bp_mode`,
 * and the `pc_dirty` flag) by pointer rather than a single owning struct,
 * so each front-end keeps its state wherever it likes (RspContext embeds
 * the fields directly; dap_session groups them) without either depending
 * on the other's context type.  Addresses are GDB-AVR addresses (the
 * 0x800000 data-space flag carried as-is); translation to UPDI happens
 * inside.  No protocol formatting happens here — callers map BpStatus to
 * their own wire reply (RSP `E08`/`E22`, DAP `verified:false`, …).
 */
#ifndef AOD_DEBUG_BP_H
#define AOD_DEBUG_BP_H

#include <stdint.h>
#include <stdbool.h>

/* AVR-Dx OCD exposes two PC comparators (BP0/BP1).  Hardware experiment
 * (this branch) showed BP1 is NOT dependable as a general user breakpoint
 * on `continue`: the same address fires on BP0 but is missed on BP1 (it
 * works only for the brief, controlled 32-bit step-over run-to-PC+4).  So
 * comparator 0 is the sole *user* HW-breakpoint slot, and comparator 1 is
 * reserved for the single-step-over of a 32-bit non-CoF instruction (so
 * stepping is always possible regardless of the user breakpoint).  A second
 * concurrent breakpoint falls back to a SW FLASH-`BREAK` (always reliable),
 * and an explicit `hbreak` (Z1) evicts an evictable `auto`-mode Z0 from the
 * user comparator so the hardware breakpoint always wins it. */
#define RSP_MAX_BREAKPOINTS    2
#define HW_BP_SLOT_EMPTY       0xFFFFFFFFu   /* empty HW-comparator shadow slot */
#define RSP_HW_BP_USER_SLOTS   1             /* comparators usable by the client */
#define RSP_HW_BP_STEP_SLOT    1             /* comparator reserved for stepping  */

/* Maximum simultaneous software breakpoints (fixed-size shadow, no heap). */
#define RSP_MAX_SW_BREAKPOINTS 64

/* Breakpoint installation policy (`monitor bp-mode` / DAP default). */
#define RSP_BP_MODE_SW       0   /* always SW FLASH-BREAK patch                 */
#define RSP_BP_MODE_HW_ONLY  1   /* Z0 aliases the HW comparator (legacy)       */
#define RSP_BP_MODE_AUTO     2   /* prefer a free HW comparator, fall back to SW*/

/* Per-session shadow entry for one software breakpoint. */
typedef struct {
    uint32_t addr;          /* GDB-side byte address (FLASH window)   */
    uint8_t  orig[2];       /* original 2-byte opcode, little-endian  */
    bool     in_use;
} RspSwBp;

/* Stop-cause classification (HLR-062).  Returned by bp_classify_stop(). */
enum RspStopCause {
    SC_NONE    = 0,
    SC_SWBREAK = 1,
    SC_HWBREAK = 2,
    SC_STEP    = 3,
    SC_INTR    = 4
};

/* Result of bp_insert()/bp_remove(); the caller maps it to its protocol. */
typedef enum {
    BP_OK         =  0,   /* installed, or idempotent no-op            */
    BP_ERR_IO     = -1,   /* UPDI I/O error            (RSP E01)       */
    BP_ERR_SLOT   = -2,   /* no free slot              (RSP E08)       */
    BP_ERR_DATASP = -3    /* data-space address, not FLASH (RSP E22)   */
} BpStatus;

/* AVR `BREAK` opcode (0x9598), little-endian byte order. */
extern const uint8_t SW_BP_BREAK_BYTES[2];

/* Snapshot/restore the CPU register file around the NVMPROG reset that a
 * FLASH `BREAK` patch forces (CPU must be halted). */
int bp_snapshot_cpu(int updi_fd, uint8_t gpr[32],
                    uint8_t *sreg, uint16_t *sp, uint32_t *pc);
int bp_restore_cpu(int updi_fd, const uint8_t gpr[32],
                   uint8_t sreg, uint16_t sp, uint32_t pc);

/* Patch `bytes` into the FLASH word at GDB byte address `byte_addr`,
 * preserving live CPU + peripheral state across the unavoidable
 * OCD→NVMPROG→OCD transition, re-arming any live HW comparators, and
 * setting *pc_dirty (HLR-054/077, LLR-RSP-37/50).  `orig_out` (optional)
 * receives the original 2 bytes.  Returns 0 or -1. */
int bp_sw_patch_flash(int updi_fd, const uint32_t hw_bp_addr[2],
                      bool *pc_dirty, uint32_t byte_addr,
                      const uint8_t bytes[2], uint8_t orig_out[2]);

/* Install a breakpoint for GDB `kind` ('0' = SW-capable, '1' = HW) at GDB
 * address `gdb_addr`, per `bp_mode` (HLR-054/055).  Idempotent.
 *
 * Two-comparator arbiter with eviction: `hw_bp_pinned[i]` marks a HW slot
 * as pinned (an explicit `Z1`/`hbreak`, or a `hw-only`-mode `Z0`) versus
 * evictable (an `auto`-mode `Z0` that merely preferred HW to avoid the SW
 * glitch).  When a `Z1` needs a comparator and both are occupied, an
 * evictable slot is converted to a SW breakpoint to make room, so an
 * explicit hardware breakpoint always wins the comparator.  Updates the
 * shadow tables, `hw_bp_pinned[]`, and *pc_dirty.  Returns a BpStatus. */
int bp_insert(int updi_fd, uint32_t hw_bp_addr[2], bool hw_bp_pinned[2],
              RspSwBp sw_bp[], int bp_mode, bool *pc_dirty,
              char kind, uint32_t gdb_addr);

/* Remove a breakpoint installed by bp_insert() (drains the SW shadow in
 * `sw`/`auto` mode, then the HW comparator shadow, clearing its pinned
 * flag).  Returns a BpStatus (BP_OK when nothing matched — a resync is
 * non-fatal). */
int bp_remove(int updi_fd, uint32_t hw_bp_addr[2], bool hw_bp_pinned[2],
              RspSwBp sw_bp[], int bp_mode, bool *pc_dirty,
              char kind, uint32_t gdb_addr);

/* (The 32-bit step-over uses the reserved comparator RSP_HW_BP_STEP_SLOT
 * directly via updi_step_32bit() — see dh_step() in gdb_rsp.c.  No borrow is
 * needed because comparator 1 is never a user slot.) */

/* Classify why the target halted: SC_SWBREAK/SC_HWBREAK when the live PC
 * matches a shadow entry, else `hint`.  Does not adjust the PC. */
int bp_classify_stop(int updi_fd, const RspSwBp sw_bp[],
                     const uint32_t hw_bp_addr[2], int hint);

/* When *pc_dirty (a fresh OCD.PC write would skip one instruction), execute
 * the instruction at PC via opcode injection before the next resume —
 * substituting the saved original opcode when a `BREAK` is patched there.
 * Leaves a direct 32-bit CALL/JMP to the caller's CoF emulation (returns 0).
 * Returns 1 when it injected a step, 0 when nothing to do, -1 on error. */
int bp_consume_pc_skip(int updi_fd, RspSwBp sw_bp[], bool *pc_dirty);

/* Single-step the instruction at the live PC, over OCD, handling every AVR
 * case the silicon needs help with: a pending fresh-PC-write / patched-`BREAK`
 * (via bp_consume_pc_skip injection), a direct 32-bit CALL/JMP (emulated as a
 * change-of-flow, leaving `*pc_dirty` set so the next resume injects the
 * skipped instruction), a 32-bit LDS/STS (stepped via the reserved comparator
 * RSP_HW_BP_STEP_SLOT at PC+4), and the ordinary 16-bit instruction (a plain
 * OCD step).  Executes exactly one instruction.  Returns 0 on success, -1 on a
 * UPDI error.  (The caller classifies the resulting stop and rebuilds any FSM
 * view.)  This is the shared core of the GDB-RSP `s`/`vCont;s` step and the
 * DAP conditional-breakpoint auto-resume. */
int bp_step_over(int updi_fd, RspSwBp sw_bp[], bool *pc_dirty);

/* Single-step (plain OCD step) while the live PC stays in the half-open
 * byte-address range [lo, hi).  Returns 0 when the PC leaves the range, 1 when
 * the BP_STEP_RANGE_MAX safety cap is hit, 2 when should_abort() (polled after
 * each step; may be NULL) returns true, or -1 on a UPDI error (target halted).
 * This is the shared core of the GDB-RSP `vCont;r` range-step (HLR-066) and the
 * DAP source-line step handlers (HLR-078) — the front-end supplies the line's
 * address range; the loop is identical. */
int bp_step_range(int updi_fd, uint32_t lo, uint32_t hi,
                  bool (*should_abort)(void *), void *abort_ctx);

/* Clear every SW-BP shadow entry without UPDI I/O (vFlashDone / reset). */
void bp_clear_all_sw(RspSwBp sw_bp[]);

#endif /* AOD_DEBUG_BP_H */
