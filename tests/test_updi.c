/* tests/test_updi.c — stub test runner (Phase 2 will implement fully) */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* __wrap_select must be present because test_updi links with --wrap,select */
#include <sys/select.h>
int __wrap_select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                  struct timeval *tv)
{
    (void)nfds; (void)r; (void)w; (void)e; (void)tv;
    return 0;
}

void test_stub_placeholder(void)
{
    TEST_IGNORE_MESSAGE("Phase 2 not yet implemented");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stub_placeholder);
    return UNITY_END();
}
