/* tests/test_install.c — verification tests for Phase 6 install/bundle.
 *
 * These tests fork() `make` sub-invocations into a private $PREFIX under
 * a temp directory and inspect the resulting file-system state.
 *
 * Tests that depend on optional host tooling (rpmbuild, ruby, man, dpkg-deb)
 * are TEST_IGNORE'd when the tool is absent, so the suite still reports
 * 8/8 across hosts with different packaging stacks installed. */
#define _POSIX_C_SOURCE 200809L

#include "unity.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── small helpers ─────────────────────────────────────────────────── */

static int run_shell(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

static bool tool_available(const char *tool)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return run_shell(cmd) == 0;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool path_executable(const char *path)
{
    return access(path, X_OK) == 0;
}

/* Build a fresh temporary $PREFIX directory and return its path in `out`.
 * Caller frees with `rm -rf` via cleanup_prefix(). */
static void make_prefix(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "/tmp/aod_install_test_%d", (int)getpid());
    /* Best-effort wipe any leftover. */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", out, out);
    (void)run_shell(cmd);
}

static void cleanup_prefix(const char *prefix)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", prefix);
    (void)run_shell(cmd);
}

/* ── Unity boilerplate ─────────────────────────────────────────────── */

void setUp(void)    { }
void tearDown(void) { }

/* ── (a) LLR-INST-01 — check-tools fails when a required tool is missing ── */
static void test_check_tools_exits_nonzero_when_required_tool_is_absent(void)
{
    /* Strategy: invoke `make check-tools` with PATH pointing at an empty
     * directory so that `avr-gcc`, `avr-nm`, etc. cannot be found.
     * `make` itself must remain reachable, so we resolve its absolute
     * path before clobbering PATH. */
    char empty[] = "/tmp/aod_emptypath_XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(empty));

    /* Capture the absolute path to make(1). */
    FILE *fp = popen("command -v make", "r");
    TEST_ASSERT_NOT_NULL(fp);
    char make_path[256] = {0};
    TEST_ASSERT_NOT_NULL(fgets(make_path, sizeof make_path, fp));
    pclose(fp);
    char *nl = strchr(make_path, '\n');
    if (nl) *nl = '\0';

    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "PATH='%s' '%s' --no-print-directory check-tools "
             ">/dev/null 2>/tmp/aod_check_tools_err",
             empty, make_path);
    int rc = run_shell(cmd);

    /* Read captured stderr to confirm a diagnostic naming a missing tool. */
    char err[2048] = {0};
    fp = fopen("/tmp/aod_check_tools_err", "r");
    if (fp) { fread(err, 1, sizeof err - 1, fp); fclose(fp); }
    unlink("/tmp/aod_check_tools_err");
    rmdir(empty);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc,
        "make check-tools must exit non-zero when required tools are absent");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, "ERROR"),
        "make check-tools must print a diagnostic on stderr");
}

/* ── (b) LLR-INST-02 — `make install` places binary at $(PREFIX)/bin ── */
static void test_make_install_places_binary_at_prefix_bin(void)
{
    char prefix[128];
    make_prefix(prefix, sizeof prefix);

    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "make --no-print-directory install PREFIX='%s' >/dev/null 2>&1",
             prefix);
    int rc = run_shell(cmd);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "make install must succeed");

    char bin_path[256];
    snprintf(bin_path, sizeof bin_path, "%s/bin/avr-updi-gdb", prefix);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(bin_path), "installed binary missing");
    TEST_ASSERT_TRUE_MESSAGE(path_executable(bin_path),
                             "installed binary not executable");

    cleanup_prefix(prefix);
}

/* ── (c) LLR-INST-02 + LLR-INST-05 — man page installed and parses ── */
static void test_make_install_places_man_page_at_prefix_man1(void)
{
    char prefix[128];
    make_prefix(prefix, sizeof prefix);

    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "make --no-print-directory install PREFIX='%s' >/dev/null 2>&1",
             prefix);
    TEST_ASSERT_EQUAL_INT(0, run_shell(cmd));

    char man_path[256];
    snprintf(man_path, sizeof man_path,
             "%s/share/man/man1/avr-updi-gdb.1", prefix);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(man_path), "installed man page missing");

    if (tool_available("man")) {
        snprintf(cmd, sizeof cmd,
                 "man -l '%s' </dev/null >/dev/null 2>&1", man_path);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, run_shell(cmd),
            "man -l must render the installed man page without error");
    } else {
        TEST_MESSAGE("man(1) not installed; skipping render check");
    }

    cleanup_prefix(prefix);
}

/* ── (d) LLR-INST-03 — uninstall removes both files and is idempotent ── */
static void test_make_uninstall_removes_all_installed_files(void)
{
    char prefix[128];
    make_prefix(prefix, sizeof prefix);

    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "make --no-print-directory install PREFIX='%s' >/dev/null 2>&1",
             prefix);
    TEST_ASSERT_EQUAL_INT(0, run_shell(cmd));

    snprintf(cmd, sizeof cmd,
             "make --no-print-directory uninstall PREFIX='%s' >/dev/null 2>&1",
             prefix);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run_shell(cmd), "uninstall must succeed");

    char bin_path[256], man_path[256];
    snprintf(bin_path, sizeof bin_path, "%s/bin/avr-updi-gdb", prefix);
    snprintf(man_path, sizeof man_path,
             "%s/share/man/man1/avr-updi-gdb.1", prefix);
    TEST_ASSERT_FALSE_MESSAGE(file_exists(bin_path),
        "binary must be removed after uninstall");
    TEST_ASSERT_FALSE_MESSAGE(file_exists(man_path),
        "man page must be removed after uninstall");

    /* Idempotency: a second uninstall on an empty prefix still exits 0. */
    snprintf(cmd, sizeof cmd,
             "make --no-print-directory uninstall PREFIX='%s' >/dev/null 2>&1",
             prefix);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run_shell(cmd),
        "second uninstall must be idempotent");

    cleanup_prefix(prefix);
}

/* ── (e) LLR-INST-04 — user manual exists with all required sections ── */
static void test_user_manual_exists_and_contains_required_sections(void)
{
    TEST_ASSERT_TRUE_MESSAGE(file_exists("doc/UserManual.md"),
        "doc/UserManual.md must exist");

    FILE *fp = fopen("doc/UserManual.md", "r");
    TEST_ASSERT_NOT_NULL(fp);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    TEST_ASSERT_NOT_NULL(buf);
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    /* Required topical headings — match the substring (case-sensitive). */
    static const char *const required[] = {
        "Prerequisites",
        "Build",
        "Connection Wiring",
        "Command-Line Invocation",
        "Examples",
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "doc/UserManual.md missing required heading: %s", required[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, required[i]), msg);
    }
    free(buf);
}

/* ── (f) LLR-INST-06 — make bundle produces a valid .deb ── */
static void test_make_bundle_produces_deb_package(void)
{
    if (!tool_available("dpkg-deb")) {
        TEST_IGNORE_MESSAGE("dpkg-deb not installed; skipping .deb bundle test");
    }
    int rc = run_shell(
        "make --no-print-directory bundle-deb VERSION=0.1.0 >/dev/null 2>&1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "make bundle-deb must succeed");

    const char *deb = "dist/avr-updi-gdb_0.1.0_amd64.deb";
    TEST_ASSERT_TRUE_MESSAGE(file_exists(deb), ".deb artefact missing");

    char cmd[256];
    snprintf(cmd, sizeof cmd, "dpkg-deb --info '%s' >/dev/null 2>&1", deb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run_shell(cmd),
        "dpkg-deb --info must succeed");

    snprintf(cmd, sizeof cmd,
             "dpkg-deb --contents '%s' 2>/dev/null | "
             "grep -q './usr/bin/avr-updi-gdb' && "
             "dpkg-deb --contents '%s' 2>/dev/null | "
             "grep -q './usr/share/man/man1/avr-updi-gdb.1'", deb, deb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run_shell(cmd),
        ".deb must contain both binary and man page");
}

/* ── (g) LLR-INST-07 — make bundle produces a valid .rpm ── */
static void test_make_bundle_produces_rpm_package(void)
{
    if (!tool_available("rpmbuild")) {
        TEST_IGNORE_MESSAGE("rpmbuild not installed; skipping .rpm bundle test");
    }
    int rc = run_shell(
        "make --no-print-directory bundle-rpm VERSION=0.1.0 >/dev/null 2>&1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "make bundle-rpm must succeed");

    const char *rpm = "dist/avr-updi-gdb-0.1.0-1.x86_64.rpm";
    TEST_ASSERT_TRUE_MESSAGE(file_exists(rpm), ".rpm artefact missing");

    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "rpm -qp --list '%s' 2>/dev/null | "
             "grep -q '/usr/bin/avr-updi-gdb' && "
             "rpm -qp --list '%s' 2>/dev/null | "
             "grep -q '/usr/share/man/man1/avr-updi-gdb.1'", rpm, rpm);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run_shell(cmd),
        ".rpm must contain both binary and man page");
}

/* ── (h) LLR-INST-08 — make bundle produces a valid Homebrew formula ── */
static void test_make_bundle_produces_homebrew_formula(void)
{
    int rc = run_shell(
        "make --no-print-directory bundle-brew VERSION=0.1.0 >/dev/null 2>&1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "make bundle-brew must succeed");

    const char *brew = "dist/avr-updi-gdb.rb";
    TEST_ASSERT_TRUE_MESSAGE(file_exists(brew), "Homebrew formula missing");

    if (tool_available("ruby")) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "ruby -c '%s' >/dev/null 2>&1", brew);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, run_shell(cmd),
            "Homebrew formula must be valid Ruby syntax");
    } else {
        TEST_MESSAGE("ruby not installed; skipping syntax check");
    }

    /* Required fields. */
    FILE *fp = fopen(brew, "r");
    TEST_ASSERT_NOT_NULL(fp);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    TEST_ASSERT_NOT_NULL(buf);
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    static const char *const fields[] = {
        "desc", "url", "sha256", "version", "def install"
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "Homebrew formula missing required field: %s", fields[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, fields[i]), msg);
    }
    free(buf);
}

/* ── runner ─────────────────────────────────────────────────────────── */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_tools_exits_nonzero_when_required_tool_is_absent);
    RUN_TEST(test_make_install_places_binary_at_prefix_bin);
    RUN_TEST(test_make_install_places_man_page_at_prefix_man1);
    RUN_TEST(test_make_uninstall_removes_all_installed_files);
    RUN_TEST(test_user_manual_exists_and_contains_required_sections);
    RUN_TEST(test_make_bundle_produces_deb_package);
    RUN_TEST(test_make_bundle_produces_rpm_package);
    RUN_TEST(test_make_bundle_produces_homebrew_formula);
    return UNITY_END();
}
