/* tests/fixtures/all_nvm.c — Phase 8 NVM fixture
 *
 * Minimal AVR128DA28 image with content placed in multiple non-FLASH
 * NVM windows so end-to-end multi-window programming can be exercised
 * by the hw-test harness (Group E / Group F).
 *
 * Sections produced:
 *   .text             — trivial program (rjmp self) so the ELF has a
 *                       FLASH segment to program alongside the others.
 *   .eeprom           — 16 bytes of recognisable EEPROM payload.
 *   .user_signatures  — 16 bytes of recognisable USERROW payload.
 *
 * FUSES and LOCK are intentionally NOT touched by this fixture:
 *   • Writing FUSES can change clock/reset/BOD configuration and
 *     potentially leave the part in an inconvenient state for the
 *     hw-test rig.
 *   • Writing LOCK requires a chip-erase and risks disabling UPDI;
 *     LOCK programming is exercised by lockbit-safety unit tests
 *     rather than on real silicon.
 *
 * The .eeprom and .user_signatures payloads each start with a
 * 4-byte ASCII tag ("EEPR"/"USER") followed by a 0..11 ramp so a
 * read-back mismatch is easy to localise.
 *
 * NOTE: avr-gcc emits .eeprom at VMA 0x00810000 and .user_signatures
 * at VMA 0x00850000 (legacy ELF NVM mapping). avr-updi-gdb
 * load_segments() does not perform an ELF-to-UPDI rebase for those
 * regions yet, so the hw-test harness invokes the per-window writers
 * directly with the fixture's payload bytes rather than running
 * `avr-updi-gdb --load` on this ELF. The fixture exists so that the
 * payload bytes are produced by avr-gcc (i.e. by the same toolchain
 * a user would use) rather than hand-coded in the test binary.
 */
#include <stdint.h>

/* Trivial main — infinite loop. The CRT provides reset vectors. */
int main(void) { for (;;) { } }

__attribute__((section(".eeprom"), used))
const uint8_t eeprom_payload[16] = {
    'E','E','P','R',  0,1,2,3,  4,5,6,7,  8,9,10,11
};

__attribute__((section(".user_signatures"), used))
const uint8_t userrow_payload[16] = {
    'U','S','E','R',  0,1,2,3,  4,5,6,7,  8,9,10,11
};
