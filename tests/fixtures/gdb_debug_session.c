/* tests/fixtures/gdb_debug_session.c
 *
 * AVR fixture for the Group-G GDB acceptance harness
 * (tests/hw/gdb_acceptance.py), Phase 14 — full interactive debug-session
 * coverage (G18+).
 *
 * Where gdb_locals.c exercises one frame's local values, this fixture
 * provides an explicit, deterministic call chain so the harness can drive
 * the rest of a real debug session against live silicon:
 *
 *     main() ── top(seed) ── mid(n) ── leaf(a, b)
 *
 * Four nested frames let the harness validate:
 *   - a multi-frame backtrace that reaches main, in order;
 *   - frame selection (`frame N`, `up`/`down`) and per-frame `info
 *     locals` / `info args`;
 *   - `step` (into), `next` (over a call), `finish` (out, with the
 *     returned value);
 *   - multiple simultaneous breakpoints and their hit ordering;
 *   - conditional breakpoints (`break leaf if b == 3`);
 *   - globals: a scalar, a struct, and an array, plus a constant marker.
 *
 * DETERMINISM: built at -O0 (see the Makefile rule, NOT the generic -Os
 * fixture rule) so every parameter and local keeps its own stack slot with
 * trivial frame-base+offset DWARF.  main() drives the chain with a CONSTANT
 * seed (SEED = 7) on every loop iteration, so the argument and local values
 * are identical on EVERY pass: main -> top(7) -> mid(7) -> leaf(7, 2) then
 * leaf(7, 3).  This matters because the harness sets its breakpoint and then
 * `continue`s — the breakpoint may not arm until after the first pass through
 * the chain, so the hit GDB actually catches is not necessarily the first
 * one.  With a constant seed every caught hit looks the same, so the verdicts
 * are robust to which iteration is observed (an earlier `7 + i` design made
 * the first caught hit timing-dependent — seed 7 vs 8 vs ...).  The functions
 * are __attribute__((noinline)) so the chain survives even if the fixture is
 * ever rebuilt at a higher optimisation level.  The harness sets breakpoints
 * by SYMBOL (break leaf/mid/top), never by line number, so it is robust to
 * edits in this file.
 *
 * Known values on every hit (used by the verdicts):
 *   g_marker      = 0xC0DE = 49374   (constant)
 *   g_cfg.base    = 100
 *   g_cfg.gain    = -7
 *   g_arr         = { 10, 20, 30, 40 }
 *   leaf(7, 2):   a=7  b=2  prod=14 sum=9   return = 14 + 9 + 49374 = 49397
 *   leaf(7, 3):   a=7  b=3  prod=21 sum=10  return = 21 + 10 + 49374 = 49405
 */
#include <stdint.h>
#include <stdbool.h>

/* ── Globals ──────────────────────────────────────────────────────────── */

/* Mutated by leaf() so a watch / re-read sees it change. */
volatile uint16_t g_counter = 0;

/* Constant marker a print must read back exactly. */
volatile uint16_t g_marker = 0xC0DE;            /* 49374 */

struct cfg { uint16_t base; int16_t gain; };
volatile struct cfg g_cfg = { 100, -7 };

volatile uint16_t g_arr[4] = { 10, 20, 30, 40 };

/* Sink so main()'s result is observably used and never elided. */
volatile uint32_t g_sink;

/* ── Call chain: main -> top -> mid -> leaf ───────────────────────────── */

/* Deepest frame. Two parameters and two locals, all live at the implicit
 * breakpoint avr-gdb places after the prologue. */
__attribute__((noinline)) uint32_t leaf(uint16_t a, uint16_t b)
{
    uint16_t prod = (uint16_t)(a * b);
    uint16_t sum  = (uint16_t)(a + b);
    g_counter++;
    return (uint32_t)prod + (uint32_t)sum + (uint32_t)g_marker;
}

/* Middle frame. Calls leaf() twice — the two call sites are the step-into
 * vs step-over subjects — and keeps a running accumulator local. */
__attribute__((noinline)) uint32_t mid(uint16_t n)
{
    uint32_t acc = 0;
    acc += leaf(n, 2);
    acc += leaf(n, 3);
    return acc + (uint32_t)g_cfg.base;
}

/* Outer frame. One local derived from mid()'s result. */
__attribute__((noinline)) uint32_t top(uint16_t seed)
{
    uint32_t r = mid(seed);
    return r + (uint32_t)(uint16_t)g_cfg.gain;
}

int main(void)
{
    /* Constant seed every iteration: the call chain's argument and local
     * values are identical on every pass, so a breakpoint hit caught on any
     * iteration (not necessarily the first) reports the same values. */
    for (;;) {
        g_sink = top(7);
    }
    return 0;
}
