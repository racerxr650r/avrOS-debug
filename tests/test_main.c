/* tests/test_main.c — stub test runner (Phase 5 will implement fully) */
#include "unity.h"
#include <sys/select.h>
#include "elf_parser.h"
#include "fsm_mapper.h"

void setUp(void) {}
void tearDown(void) {}

/* ── wrap stubs ─────────────────────────────────────────────────────────── */
int __wrap_updi_open(const char *dev, int baud)
{ (void)dev;(void)baud; return -1; }

void __wrap_updi_close(int fd) { (void)fd; }

int __wrap_updi_console_poll(int fd, char *buf, size_t cap)
{ (void)fd;(void)buf;(void)cap; return 0; }

int __wrap_rsp_listen(int port) { (void)port; return -1; }
int __wrap_rsp_accept(int lfd)  { (void)lfd;  return -1; }
void __wrap_rsp_close(int fd)   { (void)fd; }

int __wrap_rsp_recv_packet(int fd, char *buf, size_t sz)
{ (void)fd;(void)buf;(void)sz; return -1; }

int __wrap_rsp_dispatch(int fd) { (void)fd; return -1; }

int __wrap_elf_open(const char *path, ElfContext *ctx)
{ (void)path;(void)ctx; return -1; }

int __wrap_elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx)
{ (void)ctx;(void)idx; return -1; }

void __wrap_elf_close(ElfContext *ctx) { (void)ctx; }

int __wrap_fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int fd)
{ (void)ctx;(void)idx;(void)fd; return -1; }

int __wrap_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{ (void)n;(void)r;(void)w;(void)e;(void)tv; return 0; }

void test_stub_placeholder(void)
{
    TEST_IGNORE_MESSAGE("Phase 5 not yet implemented");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stub_placeholder);
    return UNITY_END();
}
