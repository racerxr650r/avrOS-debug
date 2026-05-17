/* tests/test_rsp.c — stub test runner (Phase 4 will implement fully) */
#include "unity.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "elf_parser.h"
#include "fsm_mapper.h"

void setUp(void) {}
void tearDown(void) {}

/* ── wrap stubs ─────────────────────────────────────────────────────────── */
int __wrap_updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{ (void)fd;(void)addr;(void)buf;(void)len; return -1; }

int __wrap_updi_halt(int fd) { (void)fd; return -1; }
int __wrap_updi_run(int fd)  { (void)fd; return -1; }
int __wrap_updi_step(int fd) { (void)fd; return -1; }
int __wrap_updi_nvm_write_flash(int fd, uint32_t a, const uint8_t *d, size_t l)
{ (void)fd;(void)a;(void)d;(void)l; return -1; }

int __wrap_updi_console_poll(int fd, char *buf, size_t cap)
{ (void)fd;(void)buf;(void)cap; return 0; }

int __wrap_fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int fd)
{ (void)ctx;(void)idx;(void)fd; return -1; }

int __wrap_fsm_get_registers(const FsmContext *ctx, int id, char *buf)
{ (void)ctx;(void)id;(void)buf; return -1; }

int __wrap_fsm_get_active_thread(const FsmContext *ctx)
{ (void)ctx; return -1; }

void __wrap_fsm_invalidate(FsmContext *ctx) { (void)ctx; }

int __wrap_monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx,
                            const char *cmd)
{ (void)rsp_fd;(void)updi_fd;(void)idx;(void)cmd; return -1; }

void test_stub_placeholder(void)
{
    TEST_IGNORE_MESSAGE("Phase 4 not yet implemented");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stub_placeholder);
    return UNITY_END();
}
