/* tests/fixtures/avros_break.c
 *
 * Hardware-test fixture: identical avrOS sentinel-symbol layout to
 * avros_full.c, but the firmware itself contains an explicit AVR
 * `break` opcode (0x9598) very early in `main` so the running CPU
 * traps to the OCD on its own — exercising the EXTBRK halt-reason
 * path (ASI_OCD_STATUS1 bit 4) in the GDB server.
 */
#include <stdint.h>

__asm__(
    ".pushsection FSM_TABLE,\"a\",@progbits\n"
    ".global __start_FSM_TABLE\n"
    "__start_FSM_TABLE:\n"
    "    .zero 9\n"
    ".global __stop_FSM_TABLE\n"
    "__stop_FSM_TABLE:\n"
    ".popsection\n"
);

__asm__(
    ".pushsection QUE_TABLE,\"a\",@progbits\n"
    ".global __start_QUE_TABLE\n"
    "__start_QUE_TABLE:\n"
    "    .zero 10\n"
    ".global __stop_QUE_TABLE\n"
    "__stop_QUE_TABLE:\n"
    ".popsection\n"
);

__asm__(
    ".pushsection EVNT_TABLE,\"a\",@progbits\n"
    ".global __start_EVNT_TABLE\n"
    "__start_EVNT_TABLE:\n"
    "    .zero 4\n"
    ".global __stop_EVNT_TABLE\n"
    "__stop_EVNT_TABLE:\n"
    ".popsection\n"
);

volatile void *currStateMachine = (void *)0;

int main(void)
{
    /* Trap into OCD via the AVR BREAK opcode.  On AVR-Dx with UPDI
     * connected and OCD enabled, this halts the CPU and sets
     * ASI_OCD_STATUS1.EXTBRK; the debugger reports it as SIGTRAP. */
    __asm__ volatile ("break" ::: "memory");

    /* Continue with the normal post-break body so the fixture still
     * looks like real firmware to the symbol-index parser. */
    return currStateMachine ? 1 : 0;
}
