/* tests/test_integration.c — black-box integration tests.
 *
 * Two checks are runnable today (build hygiene, runtime dependencies);
 * the live GDB-session checks (HLR-005, HLR-020) require a real or
 * emulated UPDI target and are marked TEST_IGNORE pending a fixture. */
#define _POSIX_C_SOURCE 200809L

#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

/* HLR-005 */
static void test_integration_server_ready_within_2_seconds_of_client_connect(void)
{
    TEST_IGNORE_MESSAGE("requires UPDI target/emulator fixture");
}

/* HLR-020 */
static void test_integration_server_emits_only_standard_rsp_no_ide_extensions(void)
{
    TEST_IGNORE_MESSAGE("requires UPDI target/emulator fixture");
}

/* HLR-033 — host sources compile clean with the project's C99/POSIX flags. */
static void test_build_compiles_clean_on_linux_with_c99_and_posix(void)
{
    const char *cmd =
        "D=$(mktemp -d); W=$D/w.log; rv=0; "
        "for S in src/main.c src/updi.c src/elf_parser.c src/gdb_rsp.c "
        "src/fsm_mapper.c src/monitor.c; do "
        "  gcc -std=c99 -D_POSIX_C_SOURCE=200809L "
        "      -Wall -Wextra -Wpedantic -Wstrict-prototypes "
        "      -Wmissing-prototypes -Wshadow -O2 -Isrc -c \"$S\" "
        "      -o \"$D/$(basename $S .c).o\" 2>>\"$W\" || rv=1; "
        "done; "
        "if grep -E 'warning:|error:' \"$W\" >/dev/null; then rv=1; "
        "  cat \"$W\" >&2; fi; "
        "rm -rf \"$D\"; exit $rv";
    int rc = system(cmd);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/* HLR-034 — runtime depends only on libc / loader / vDSO */
static void test_runtime_links_only_libc_no_heavyweight_deps(void)
{
    FILE *fp = popen("ldd build/avr-updi-gdb", "r");
    TEST_ASSERT_NOT_NULL(fp);
    char line[512];
    int bad = 0;
    while (fgets(line, sizeof line, fp)) {
        /* Allowed: libc.so, libutil.so (forkpty), ld-linux*, linux-vdso,
         * and the static "linux-gate" / not-a-dynamic-exec lines. */
        if (strstr(line, "libc.so"))       continue;
        if (strstr(line, "libutil.so"))    continue;
        if (strstr(line, "ld-linux"))      continue;
        if (strstr(line, "linux-vdso"))    continue;
        if (strstr(line, "linux-gate"))    continue;
        if (strstr(line, "statically linked")) continue;
        if (strstr(line, "not a dynamic")) continue;
        /* blank lines & summary noise */
        if (line[0] == '\n' || line[0] == '\0') continue;
        fprintf(stderr, "unexpected dep: %s", line);
        bad++;
    }
    pclose(fp);
    TEST_ASSERT_EQUAL_INT(0, bad);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_integration_server_ready_within_2_seconds_of_client_connect);
    RUN_TEST(test_integration_server_emits_only_standard_rsp_no_ide_extensions);
    RUN_TEST(test_build_compiles_clean_on_linux_with_c99_and_posix);
    RUN_TEST(test_runtime_links_only_libc_no_heavyweight_deps);
    return UNITY_END();
}
