/* tests/fixtures/gdb_locals.c
 *
 * AVR fixture for the Group-G GDB acceptance harness
 * (tests/hw/gdb_acceptance.py), test G17 — local-variable value
 * correctness.
 *
 * locals_probe() declares one local of each representative C data type
 * (8/16/32/64-bit signed & unsigned ints, char, bool, float, an array, a
 * struct, a char-array string, and a pointer-to-global) and initialises
 * each to a distinctive, known constant.  G17 breaks INSIDE locals_probe
 * (on the `g_probe_hit = 1;` anchor line, where every local is assigned
 * and still live) and checks that each reported value matches its
 * constant — so a bug in avrOSdb's SP read or SRAM read path (the kind
 * that makes GDB/Cortex-Debug show garbage locals while stepping) is
 * caught.
 *
 * The probe reads its own (current) frame: address = frame-pointer (Y,
 * r28:r29) + offset, then a data-space read.  It deliberately does NOT
 * rely on unwinding to a *caller* frame — avr-gdb's AVR unwinder cannot
 * recover a parent frame from a nested leaf on this target, which is a
 * separate concern from local-value correctness.
 *
 * Built at -O0 (see the Makefile rule, NOT the generic -Os fixture rule):
 * every local then gets its own stack slot with trivial frame-base+offset
 * DWARF, so the reported values are deterministic and test the host's
 * memory/register plumbing rather than the compiler's optimiser.  At -Os
 * most of these locals would be coalesced or elided and the test would be
 * about GCC, not avrOSdb.
 */
#include <stdint.h>
#include <stdbool.h>

/* A known global in SRAM that a local pointer dereferences (avoids the
 * AVR Harvard flash-string complication of `const char *s = "..."`). */
volatile uint16_t g_marker = 0xCAFE;        /* 51966 */

/* Anchor: G17 breaks on the source line that writes this, at which point
 * every local in locals_probe() is assigned and still live. */
volatile uint8_t g_probe_hit;

/* Sink that consumes every local so none is unused/elided even if the
 * fixture is ever rebuilt at a higher optimisation level. */
volatile uint32_t g_locals_sink;

struct point { int16_t x; int16_t y; };

__attribute__((noinline)) void locals_probe(void)
{
    uint8_t   u8   = 0xA5;                   /* 165              */
    int8_t    i8   = -42;                    /* -42              */
    uint16_t  u16  = 0xBEEF;                 /* 48879            */
    int16_t   i16  = -12345;                 /* -12345           */
    uint32_t  u32  = 0xDEADBEEF;             /* 3735928559       */
    int32_t   i32  = -123456789;             /* -123456789       */
    uint64_t  u64  = 0x0123456789ABCDEFULL;  /* 81985529216486895 */
    char      ch   = 'Q';                    /* 81 'Q'           */
    bool      flag = true;                   /* true             */
    float     f    = 3.5f;                   /* 3.5              */
    uint16_t  arr[4] = { 0x1111, 0x2222, 0x3333, 0x4444 }; /* 4369,8738,13107,17476 */
    struct point pt = { 1000, -2000 };       /* {x=1000, y=-2000} */
    char      name[6] = "avrOS";             /* "avrOS"          */
    uint16_t *pmark = (uint16_t *)&g_marker; /* *pmark == 51966  */

    /* G17 BREAKPOINT ANCHOR — keep this on its own line. The matching
     * `break gdb_locals.c:<line>` in tests/hw/gdb_acceptance.py (G17) must
     * point here: all locals above are assigned, none consumed yet. */
    g_probe_hit = 1;

    g_locals_sink = (uint32_t)u8 + (uint32_t)(uint8_t)i8 + u16
                  + (uint32_t)(uint16_t)i16 + u32 + (uint32_t)i32
                  + (uint32_t)u64 + (uint8_t)ch + flag + (uint32_t)f
                  + arr[0] + arr[3] + (uint16_t)pt.x + (uint16_t)pt.y
                  + (uint8_t)name[0] + *pmark;
}

int main(void)
{
    for (;;)
        locals_probe();
    return 0;
}
