/* tests/ocd_stubs.c — no-op OCD/NVM + DWARF-line stubs.
 *
 * The breakpoint core (src/debug_bp.c) and the DAP front-end (src/dap.c) call
 * a handful of UPDI OCD/NVM primitives and elf_line_to_addr().  Test binaries
 * that link dap.c / debug_bp.c but NOT the real updi.c / elf_parser.c (e.g.
 * test_dap, test_main) link this TU so those references resolve to harmless
 * no-ops.  All breakpoint dispatch in those unit tests runs with updi_fd = -1,
 * so these are never actually exercised — they only satisfy the linker.
 *
 * NOTE: updi_ocd_read_pc() is deliberately NOT defined here — test_dap and
 * test_main provide their own (it is part of their stop/step behaviour).
 */
#include <stddef.h>
#include <stdint.h>
#include "updi.h"
#include "elf_parser.h"

int updi_enter_debug(int fd) { (void)fd; return 0; }
int updi_mem_read(int fd, uint32_t a, uint8_t *b, size_t n)
{ (void)fd; (void)a; if (b) for (size_t i = 0; i < n; i++) b[i] = 0; return 0; }
int updi_nvm_flash_patch(int fd, uint32_t a, const uint8_t *b, size_t n)
{ (void)fd; (void)a; (void)b; (void)n; return 0; }
int updi_ocd_set_hw_bp(int fd, int slot, uint32_t a) { (void)fd; (void)slot; (void)a; return 0; }
int updi_ocd_clear_hw_bp(int fd, int slot) { (void)fd; (void)slot; return 0; }
int updi_ocd_read_gpr(int fd, uint8_t n, uint8_t *v) { (void)fd; (void)n; if (v) *v = 0; return 0; }
int updi_ocd_write_gpr(int fd, uint8_t n, uint8_t v) { (void)fd; (void)n; (void)v; return 0; }
int updi_ocd_read_sreg(int fd, uint8_t *v) { (void)fd; if (v) *v = 0; return 0; }
int updi_ocd_write_sreg(int fd, uint8_t v) { (void)fd; (void)v; return 0; }
int updi_ocd_read_sp(int fd, uint16_t *v) { (void)fd; if (v) *v = 0; return 0; }
int updi_ocd_write_sp(int fd, uint16_t v) { (void)fd; (void)v; return 0; }
int updi_ocd_write_pc(int fd, uint32_t a) { (void)fd; (void)a; return 0; }
int updi_ocd_stabilize_pc_after_write(int fd) { (void)fd; return 0; }
int updi_ocd_step_inject_word0(int fd, uint16_t w) { (void)fd; (void)w; return 0; }
int updi_step_32bit(int fd, int slot, uint32_t tgt, bool hoj) { (void)fd; (void)slot; (void)tgt; (void)hoj; return 0; }
int updi_ocd_emulate_cof_32bit(int fd, uint32_t ret, uint32_t tgt) { (void)fd; (void)ret; (void)tgt; return 0; }
int updi_save_peripherals(int fd, uint8_t *b) { (void)fd; (void)b; return 0; }
int updi_restore_peripherals(int fd, const uint8_t *b) { (void)fd; (void)b; return 0; }

int elf_line_to_addr(const ElfContext *c, const char *f, int line, uint32_t *a)
{ (void)c; (void)f; (void)line; (void)a; return -1; }   /* unresolved in unit tests */

int elf_cfi_cfa(const ElfContext *c, uint32_t a, int *reg, int *off)
{ (void)c; (void)a; (void)reg; (void)off; return -1; } /* no CFI in unit tests */

int elf_var_addr(const ElfContext *c, uint32_t pc, const ElfFrameRegs *fr,
                 const char *n, uint32_t *a, int *sz, bool *sg)
{ (void)c; (void)pc; (void)fr; (void)n; (void)a; (void)sz; (void)sg; return -1; }

int elf_addr_to_func(const ElfContext *c, uint32_t pc, char *n, size_t cap)
{ (void)c; (void)pc; (void)n; (void)cap; return -1; }
