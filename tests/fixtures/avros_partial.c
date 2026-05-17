/* tests/fixtures/avros_partial.c
 *
 * Exports only FSM_TABLE sentinels + currStateMachine.  QUE_TABLE and
 * EVNT_TABLE intentionally absent so elf_find_avros_tables can be
 * tested for graceful degradation (must return 0, not -1, and leave
 * absent fields zero-initialised).
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

volatile void *currStateMachine = (void *)0;

int main(void) { return currStateMachine ? 1 : 0; }
