/* tests/fixtures/gdb_target.c
 *
 * Tiny AVR fixture used by the Group-G GDB acceptance harness
 * (tests/hw/gdb_acceptance.py).
 *
 * Provides:
 *   - main()        Symbol GDB can `break` on after `load`.
 *   - blink()       Non-inlined function whose body writes the watched
 *                   variable; gives the watchpoint test something to
 *                   fire on.
 *   - g_counter     volatile uint8_t used as the watch target. Marked
 *                   volatile so the optimiser cannot fold the write away
 *                   even at -Os.
 *
 * Intentionally tiny so .text only occupies the lowest FLASH page —
 * leaving plenty of erased space for "set 5 breakpoints at scattered
 * FLASH addresses" without trampling real instructions.
 */
#include <stdint.h>

volatile uint8_t g_counter;

__attribute__((noinline)) void blink(void)
{
    g_counter++;
}

int main(void)
{
    while (1) {
        blink();
    }
}
