/* tests/test_integration.c — stub test runner (Phase 5 will implement fully)
 * Integration tests launch the real avr-updi-gdb binary via fork()/execv().
 * No --wrap flags needed.
 */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

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
