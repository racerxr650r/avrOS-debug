/* tests/fixtures/wrong_family.c
 *
 * LLR-MAIN-13 hardware fixture — built with `-mmcu=avr64dd32`.  The
 * AVR-DD device-pack header avr-gcc embeds an
 * `.note.gnu.avr.deviceinfo` ELF note containing the lowercase
 * part-name string "avr64dd32".  When this ELF is fed to
 * `avrOSdb` while attached to the bench AVR128DA28 board
 * (`/dev/ttyAMA2`), `elf_open()` populates
 * `ElfContext.device_name = "avr64dd32"` which
 * `updi_family_from_partname()` classifies as "AVR-DD".  SIGROW
 * autodetect on the real silicon then reports "AVR-DA", so
 * `app_main()` must abort with the LLR-MAIN-13 mismatch diagnostic:
 *
 *     error: ELF was built for avr64dd32 (family AVR-DD) but target
 *     reports AVR-DA (use --force-device=AVR-DA to override)
 *
 * and exit with rc=1 before opening the GDB listener.  Re-running with
 * `--force-device=AVR-DA` exercises the documented override path: the
 * mismatch check is suppressed and the tool proceeds normally.
 *
 * The payload is intentionally trivial — only the ELF header, a single
 * PT_LOAD segment, and the deviceinfo note are needed.  We do NOT
 * include avrOS sentinel symbols because the mismatch abort fires
 * before `elf_find_avros_tables()` runs.                              */
#include <stdint.h>

volatile uint8_t hw_mismatch_canary = 0xA5u;

int main(void)
{
    /* Touch the canary so -Os does not GC the symbol; gives the
     * resulting ELF a non-empty .data section.                       */
    hw_mismatch_canary ^= 0x5Au;
    while (1) { /* spin */ }
    return 0;
}
