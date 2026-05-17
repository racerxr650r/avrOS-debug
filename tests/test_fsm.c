/* tests/test_fsm.c — stub test runner (Phase 3 will implement fully) */
#include "unity.h"
#include <stdint.h>
#include <stddef.h>

void setUp(void) {}
void tearDown(void) {}

/* __wrap_updi_mem_read must be present because test_fsm links --wrap,updi_mem_read */
int __wrap_updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)fd; (void)addr; (void)buf; (void)len;
    return -1;
}

void test_stub_placeholder(void)
{
    TEST_IGNORE_MESSAGE("Phase 3 not yet implemented");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stub_placeholder);
    return UNITY_END();
}
