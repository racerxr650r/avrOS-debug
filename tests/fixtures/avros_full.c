/* tests/fixtures/avros_full.c
 * Minimal AVR C source that exports all 8 avrOS sentinel linker symbols.
 * Compiled with avr-gcc to produce a valid AVR ELF32 fixture.
 */
#include <stdint.h>

/* FSM table sentinels */
__attribute__((section(".avros_fsm_table")))
uint32_t __avros_fsm_table_start = 0;
__attribute__((section(".avros_fsm_table")))
uint32_t __avros_fsm_table_end   = 0;

/* Queue table sentinels */
__attribute__((section(".avros_queue_table")))
uint32_t __avros_queue_table_start = 0;
__attribute__((section(".avros_queue_table")))
uint32_t __avros_queue_table_end   = 0;

/* Event mask (SRAM) */
volatile uint32_t __avros_event_mask = 0;

/* Mempool table sentinels */
__attribute__((section(".avros_mempool_table")))
uint32_t __avros_mempool_table_start = 0;
__attribute__((section(".avros_mempool_table")))
uint32_t __avros_mempool_table_end   = 0;

/* Current FSM pointer (SRAM) */
volatile uint32_t __avros_current_fsm = 0;

int main(void) { return 0; }
