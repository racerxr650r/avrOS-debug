/* tests/test_elf.c — stub test runner (Phase 1 will implement fully)
 * Currently just verifies that the build infrastructure works.
 */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_stub_placeholder(void)
{
    TEST_IGNORE_MESSAGE("Phase 1 not yet implemented");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stub_placeholder);
    return UNITY_END();
}
