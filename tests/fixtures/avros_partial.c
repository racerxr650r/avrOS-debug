/* tests/fixtures/avros_partial.c
 * AVR C source that exports only 4 of the 8 avrOS sentinel symbols.
 * Used to test graceful degradation in elf_find_avros_tables().
 */
#include <stdint.h>

/* FSM table sentinels only */
__attribute__((section(".avros_fsm_table")))
uint32_t __avros_fsm_table_start = 0;
__attribute__((section(".avros_fsm_table")))
uint32_t __avros_fsm_table_end   = 0;

/* Event mask (SRAM) */
volatile uint32_t __avros_event_mask = 0;

/* Current FSM pointer (SRAM) */
volatile uint32_t __avros_current_fsm = 0;

/* Queue and mempool sentinels intentionally absent */

int main(void) { return 0; }
