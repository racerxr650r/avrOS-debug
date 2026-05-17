/* tests/fixtures/not_avr.c
 * Minimal C source compiled for the host architecture (x86-64 or i386).
 * The resulting ELF has e_machine != EM_AVR (0x0053), so elf_open() must
 * reject it.  Used by test_elf to verify the EM_AVR guard.
 */
int main(void) { return 0; }
