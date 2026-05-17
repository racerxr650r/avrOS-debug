/* tests/fixtures/avros_full.c
 *
 * Minimal AVR C source that exports all avrOS sentinel linker symbols
 * referenced by elf_find_avros_tables() — matching the real avrOS layout
 * (see app/avrOS_example/avrOS.x and sys/fsm.[ch]):
 *
 *   FSM_TABLE   section  → __start_FSM_TABLE  / __stop_FSM_TABLE
 *   QUE_TABLE   section  → __start_QUE_TABLE  / __stop_QUE_TABLE
 *   EVNT_TABLE  section  → __start_EVNT_TABLE / __stop_EVNT_TABLE
 *   currStateMachine     → SRAM-resident pointer
 *
 * Per-entry strides match the real avrOS descriptor sizes:
 *   fsmStateMachineDescr_t = 9 bytes
 *   queDescriptor_t (no QUE_STATS) = 10 bytes
 *   evntDescriptor_t = 4 bytes
 *
 * The default avr-gcc linker script does NOT emit __start_/__stop_
 * boundary symbols for arbitrary orphan sections, so each table is
 * emitted as a single inline-asm block placing start label, entry
 * payload, and stop label adjacently within one section contribution
 * (preserving size = stop - start).
 */
#include <stdint.h>

__asm__(
    ".pushsection FSM_TABLE,\"a\",@progbits\n"
    ".global __start_FSM_TABLE\n"
    "__start_FSM_TABLE:\n"
    "    .zero 9\n"               /* one fsmStateMachineDescr_t */
    ".global __stop_FSM_TABLE\n"
    "__stop_FSM_TABLE:\n"
    ".popsection\n"
);

__asm__(
    ".pushsection QUE_TABLE,\"a\",@progbits\n"
    ".global __start_QUE_TABLE\n"
    "__start_QUE_TABLE:\n"
    "    .zero 10\n"              /* one queDescriptor_t (no QUE_STATS) */
    ".global __stop_QUE_TABLE\n"
    "__stop_QUE_TABLE:\n"
    ".popsection\n"
);

__asm__(
    ".pushsection EVNT_TABLE,\"a\",@progbits\n"
    ".global __start_EVNT_TABLE\n"
    "__start_EVNT_TABLE:\n"
    "    .zero 4\n"               /* one evntDescriptor_t */
    ".global __stop_EVNT_TABLE\n"
    "__stop_EVNT_TABLE:\n"
    ".popsection\n"
);

/* SRAM-resident "current state machine" pointer.  In real avrOS this is
 * a file-local static; here we make it global so the symbol appears in
 * .symtab with the same name. */
volatile void *currStateMachine = (void *)0;

int main(void) { return currStateMachine ? 1 : 0; }
