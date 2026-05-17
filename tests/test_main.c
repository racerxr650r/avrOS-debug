/* tests/test_main.c — Unity tests for src/main.c (LLR-MAIN-01 .. 07).
 *
 * Strategy:
 *   • #include "main.c" so the (UNIT_TEST-exposed) helpers parse_args /
 *     event_loop / load_flash_segments / app_main are reachable directly
 *     and the file-scope g_quit flag is observable.
 *   • Wrap every external symbol main.c touches (updi_*, rsp_*, elf_*,
 *     fsm_*, select) with a recording shim. Test bodies arm the shims,
 *     invoke the target, and inspect the captured state.
 *   • For tests that exercise exit() paths (LLR-MAIN-01/02 error branches)
 *     we fork() and inspect the child's exit status. */
#define _POSIX_C_SOURCE 200809L

#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __linux__
#include <elf.h>
#else
#include "elf.h"
#endif

#include "elf_parser.h"
#include "updi.h"
#include "fsm_mapper.h"
#include "gdb_rsp.h"

/* main.c is brought in so parse_args/event_loop/load_flash_segments and
 * the renamed app_main() are visible. */
#include "main.c"

/* ── Mock state ──────────────────────────────────────────────────────── */
typedef enum {
    CALL_NONE = 0,
    CALL_UPDI_OPEN, CALL_UPDI_CLOSE, CALL_UPDI_CONSOLE_POLL,
    CALL_UPDI_NVM,
    CALL_RSP_LISTEN, CALL_RSP_ACCEPT, CALL_RSP_CLOSE,
    CALL_RSP_RECV, CALL_RSP_DISPATCH,
    CALL_ELF_OPEN, CALL_ELF_TABLES, CALL_ELF_CLOSE,
    CALL_FSM_BUILD, CALL_SELECT,
    CALL_RSP_DEFAULT_HANDLERS,
} CallId;

#define LOG_MAX 64
static CallId  call_log[LOG_MAX];
static int     call_log_len;
#define LOG(id) do { if (call_log_len < LOG_MAX) call_log[call_log_len++] = (id); } while (0)

static int mk_updi_open_ret;
static int mk_updi_open_calls;
static int mk_updi_close_calls;
static int mk_updi_nvm_ret;
static int mk_updi_nvm_calls;

static int mk_rsp_listen_ret;
static int mk_rsp_listen_calls;
static int mk_rsp_accept_ret;
static int mk_rsp_close_calls;
static int mk_rsp_recv_ret;
static int mk_rsp_dispatch_calls;

static int  mk_elf_open_ret;
static int  mk_elf_open_calls;
static int  mk_elf_close_calls;
static int  mk_elf_tmp_fd;
static uint32_t mk_elf_planted_sram;
static Elf32_Ehdr mk_elf_planted_ehdr;
static int  mk_elf_have_ehdr;

static int  mk_select_ret;
static int  mk_select_errno;
static int  mk_select_calls;
static int  mk_select_set_g_quit;
static int  mk_select_nfds;
static int  mk_select_listen_in;
static int  mk_select_gdb_in;
static int  mk_select_updi_in;
static int  mk_select_listen_fd;
static int  mk_select_gdb_fd;
static int  mk_select_updi_fd;
static int  mk_select_force_listen_ready;
static int  mk_select_force_gdb_ready;
static int  mk_select_force_updi_ready;

/* ── Wraps ───────────────────────────────────────────────────────────── */
int __wrap_updi_open(const char *dev, int baud);
int __wrap_updi_open(const char *dev, int baud)
{
    (void)dev; (void)baud;
    mk_updi_open_calls++;
    LOG(CALL_UPDI_OPEN);
    return mk_updi_open_ret;
}

void __wrap_updi_close(int fd);
void __wrap_updi_close(int fd)
{
    (void)fd;
    mk_updi_close_calls++;
    LOG(CALL_UPDI_CLOSE);
}

int __wrap_updi_console_poll(int fd, char *buf, size_t cap);
int __wrap_updi_console_poll(int fd, char *buf, size_t cap)
{
    (void)fd; (void)buf; (void)cap;
    LOG(CALL_UPDI_CONSOLE_POLL);
    return 0;
}

/* updi.c is not linked into test_main; main.c references
 * updi_read_device_info() from run_device_mode(). The --device path
 * is exercised by tests/test_device.c, so a simple stub satisfies the
 * link here. */
int updi_read_device_info(int fd, UpdiDeviceInfo *info);
int updi_read_device_info(int fd, UpdiDeviceInfo *info)
{
    (void)fd;
    if (info) { info->fail_op = "stub"; info->fail_errno = -1; }
    return -1;
}

int __wrap_updi_nvm_write_flash(int fd, uint32_t a, const uint8_t *d, size_t n);
int __wrap_updi_nvm_write_flash(int fd, uint32_t a, const uint8_t *d, size_t n)
{
    (void)fd; (void)a; (void)d; (void)n;
    mk_updi_nvm_calls++;
    LOG(CALL_UPDI_NVM);
    return mk_updi_nvm_ret;
}

int __wrap_rsp_listen(uint16_t port);
int __wrap_rsp_listen(uint16_t port)
{
    (void)port;
    mk_rsp_listen_calls++;
    LOG(CALL_RSP_LISTEN);
    return mk_rsp_listen_ret;
}

int __wrap_rsp_accept(int listen_fd);
int __wrap_rsp_accept(int listen_fd)
{
    (void)listen_fd;
    LOG(CALL_RSP_ACCEPT);
    return mk_rsp_accept_ret;
}

void __wrap_rsp_close(int fd);
void __wrap_rsp_close(int fd)
{
    (void)fd;
    mk_rsp_close_calls++;
    LOG(CALL_RSP_CLOSE);
}

int __wrap_rsp_recv_packet(int fd, char *buf, size_t cap);
int __wrap_rsp_recv_packet(int fd, char *buf, size_t cap)
{
    (void)fd; (void)cap;
    if (mk_rsp_recv_ret > 0) { buf[0] = 'g'; buf[1] = '\0'; }
    LOG(CALL_RSP_RECV);
    return mk_rsp_recv_ret;
}

int __wrap_rsp_dispatch(int fd, const char *packet, RspHandlers *h);
int __wrap_rsp_dispatch(int fd, const char *packet, RspHandlers *h)
{
    (void)fd; (void)packet; (void)h;
    mk_rsp_dispatch_calls++;
    LOG(CALL_RSP_DISPATCH);
    return 0;
}

void __wrap_rsp_default_handlers(RspHandlers *h, RspContext *ctx);
void __wrap_rsp_default_handlers(RspHandlers *h, RspContext *ctx)
{
    (void)ctx;
    memset(h, 0, sizeof *h);
    LOG(CALL_RSP_DEFAULT_HANDLERS);
}

int __wrap_elf_open(const char *path, ElfContext *ctx);
int __wrap_elf_open(const char *path, ElfContext *ctx)
{
    (void)path;
    mk_elf_open_calls++;
    LOG(CALL_ELF_OPEN);
    memset(ctx, 0, sizeof *ctx);
    ctx->fd = mk_elf_tmp_fd;
    if (mk_elf_have_ehdr) ctx->ehdr = mk_elf_planted_ehdr;
    ctx->sram_base = mk_elf_planted_sram;
    return mk_elf_open_ret;
}

void __wrap_elf_close(ElfContext *ctx);
void __wrap_elf_close(ElfContext *ctx)
{
    if (ctx && ctx->fd >= 0) close(ctx->fd);
    mk_elf_close_calls++;
    LOG(CALL_ELF_CLOSE);
}

int __wrap_elf_find_avros_tables(ElfContext *c, AvrOsSymbolIndex *i);
int __wrap_elf_find_avros_tables(ElfContext *c, AvrOsSymbolIndex *i)
{
    (void)c; (void)i;
    LOG(CALL_ELF_TABLES);
    return 0;
}

int __wrap_fsm_build_thread_list(FsmContext *c, const AvrOsSymbolIndex *i, int fd);
int __wrap_fsm_build_thread_list(FsmContext *c, const AvrOsSymbolIndex *i, int fd)
{
    (void)c; (void)i; (void)fd;
    LOG(CALL_FSM_BUILD);
    return 0;
}

int __wrap_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
                  struct timeval *tv);
int __wrap_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
                  struct timeval *tv)
{
    (void)wfds; (void)efds; (void)tv;
    mk_select_calls++;
    mk_select_nfds = nfds;
    if (rfds) {
        if (mk_select_listen_fd >= 0 && FD_ISSET(mk_select_listen_fd, rfds))
            mk_select_listen_in = 1;
        if (mk_select_gdb_fd    >= 0 && FD_ISSET(mk_select_gdb_fd,    rfds))
            mk_select_gdb_in    = 1;
        if (mk_select_updi_fd   >= 0 && FD_ISSET(mk_select_updi_fd,   rfds))
            mk_select_updi_in   = 1;

        FD_ZERO(rfds);
        if (mk_select_force_listen_ready && mk_select_listen_fd >= 0)
            FD_SET(mk_select_listen_fd, rfds);
        if (mk_select_force_gdb_ready && mk_select_gdb_fd >= 0)
            FD_SET(mk_select_gdb_fd, rfds);
        if (mk_select_force_updi_ready && mk_select_updi_fd >= 0)
            FD_SET(mk_select_updi_fd, rfds);
    }
    if (mk_select_set_g_quit && mk_select_calls >= mk_select_set_g_quit) g_quit = 1;
    errno = mk_select_errno;
    return mk_select_ret;
}

/* ── Unity hooks ─────────────────────────────────────────────────────── */
void setUp(void)
{
    memset(call_log, 0, sizeof call_log);
    call_log_len = 0;

    mk_updi_open_ret = 7;       mk_updi_open_calls = 0;
    mk_updi_close_calls = 0;
    mk_updi_nvm_ret = 0;        mk_updi_nvm_calls = 0;

    mk_rsp_listen_ret = 8;      mk_rsp_listen_calls = 0;
    mk_rsp_accept_ret = 9;
    mk_rsp_close_calls = 0;
    mk_rsp_recv_ret = 0;        mk_rsp_dispatch_calls = 0;

    mk_elf_open_ret = 0;        mk_elf_open_calls = 0;
    mk_elf_close_calls = 0;
    mk_elf_tmp_fd = -1;
    mk_elf_planted_sram = 0;
    mk_elf_have_ehdr = 0;
    memset(&mk_elf_planted_ehdr, 0, sizeof mk_elf_planted_ehdr);

    mk_select_ret = -1;
    mk_select_errno = EINTR;
    mk_select_calls = 0;
    mk_select_set_g_quit = 1;
    mk_select_listen_in = mk_select_gdb_in = mk_select_updi_in = 0;
    mk_select_listen_fd = mk_select_gdb_fd = mk_select_updi_fd = -1;
    mk_select_force_listen_ready = 0;
    mk_select_force_gdb_ready = 0;
    mk_select_force_updi_ready = 0;

    g_quit = 0;
}

void tearDown(void) { }

/* ── Helpers ─────────────────────────────────────────────────────────── */
static int run_in_child(int (*fn)(int, char **), int argc, char **argv)
{
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        _exit(fn(argc, argv));
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Build a tiny ELF32-AVR file with N program headers and return an open
 * fd. The header's e_phoff/e_phnum are populated so load_flash_segments
 * can walk the table; payloads are zero-filled. */
static int build_min_elf(uint32_t *vmas, uint32_t *sizes, int n,
                         Elf32_Ehdr *out_ehdr)
{
    char path[] = "/tmp/aod_test_elfXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);

    Elf32_Ehdr eh;
    memset(&eh, 0, sizeof eh);
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS]   = ELFCLASS32;
    eh.e_type      = ET_EXEC;
    eh.e_machine   = EM_AVR;
    eh.e_phoff     = sizeof eh;
    eh.e_phentsize = sizeof(Elf32_Phdr);
    eh.e_phnum     = (uint16_t)n;
    eh.e_ehsize    = sizeof eh;
    if (write(fd, &eh, sizeof eh) != (ssize_t)sizeof eh) { close(fd); return -1; }

    off_t data_off = (off_t)sizeof eh + (off_t)n * (off_t)sizeof(Elf32_Phdr);
    for (int i = 0; i < n; i++) {
        Elf32_Phdr ph;
        memset(&ph, 0, sizeof ph);
        ph.p_type   = PT_LOAD;
        ph.p_offset = (Elf32_Off)data_off;
        ph.p_vaddr  = vmas[i];
        ph.p_paddr  = vmas[i];
        ph.p_filesz = sizes[i];
        ph.p_memsz  = sizes[i];
        if (write(fd, &ph, sizeof ph) != (ssize_t)sizeof ph) { close(fd); return -1; }
        data_off += sizes[i];
    }
    for (int i = 0; i < n; i++) {
        if (sizes[i] == 0) continue;
        uint8_t *z = calloc(1, sizes[i]);
        if (write(fd, z, sizes[i]) != (ssize_t)sizes[i]) {
            free(z); close(fd); return -1;
        }
        free(z);
    }
    if (out_ehdr) *out_ehdr = eh;
    return fd;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

/* LLR-MAIN-01 */
static void test_parse_args_valid_positional_args_populate_config(void)
{
    char *argv[] = { (char*)"prog", (char*)"/dev/ttyUSB0", (char*)"a.elf" };
    AppConfig cfg;
    parse_args(3, argv, &cfg);
    TEST_ASSERT_EQUAL_STRING("/dev/ttyUSB0", cfg.serial_device);
    TEST_ASSERT_EQUAL_STRING("a.elf",        cfg.elf_path);
}

static int call_parse_args(int argc, char **argv)
{ AppConfig cfg; parse_args(argc, argv, &cfg); return 0; }

/* LLR-MAIN-01 */
static void test_parse_args_unrecognised_flag_exits_1(void)
{
    char *argv[] = { (char*)"prog", (char*)"--bogus",
                     (char*)"/dev/x", (char*)"a.elf" };
    TEST_ASSERT_EQUAL_INT(1, run_in_child(call_parse_args, 4, argv));
}

/* LLR-MAIN-02 */
static void test_parse_args_default_port_baud_load_when_not_supplied(void)
{
    char *argv[] = { (char*)"prog", (char*)"/dev/x", (char*)"a.elf" };
    AppConfig cfg;
    parse_args(3, argv, &cfg);
    TEST_ASSERT_EQUAL_UINT16(1234, cfg.gdb_port);
    TEST_ASSERT_EQUAL_INT(115200,  cfg.baud_rate);
    TEST_ASSERT_FALSE(cfg.load_flash);
}

/* LLR-MAIN-02 */
static void test_parse_args_missing_serial_device_exits_1(void)
{
    char *argv[] = { (char*)"prog" };
    TEST_ASSERT_EQUAL_INT(1, run_in_child(call_parse_args, 1, argv));
}

/* LLR-MAIN-02 */
static void test_parse_args_missing_elf_file_exits_1(void)
{
    char *argv[] = { (char*)"prog", (char*)"/dev/x" };
    TEST_ASSERT_EQUAL_INT(1, run_in_child(call_parse_args, 2, argv));
}

/* LLR-MAIN-03 */
static void test_main_updi_open_failure_releases_resources_and_returns_1(void)
{
    uint32_t v[] = {0}, s[] = {0};
    mk_elf_tmp_fd = build_min_elf(v, s, 1, &mk_elf_planted_ehdr);
    mk_elf_have_ehdr = 1;
    mk_updi_open_ret = -1;

    char *argv[] = { (char*)"prog", (char*)"/dev/x", (char*)"a.elf" };
    int rc = app_main(3, argv);
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_INT(1, mk_updi_open_calls);
    TEST_ASSERT_EQUAL_INT(0, mk_rsp_listen_calls);
    TEST_ASSERT_EQUAL_INT(1, mk_elf_close_calls);
}

/* LLR-MAIN-03 */
static void test_main_rsp_listen_failure_closes_updi_and_returns_1(void)
{
    uint32_t v[] = {0}, s[] = {0};
    mk_elf_tmp_fd = build_min_elf(v, s, 1, &mk_elf_planted_ehdr);
    mk_elf_have_ehdr = 1;
    mk_rsp_listen_ret = -1;

    char *argv[] = { (char*)"prog", (char*)"/dev/x", (char*)"a.elf" };
    TEST_ASSERT_EQUAL_INT(1, app_main(3, argv));
    TEST_ASSERT_EQUAL_INT(1, mk_rsp_listen_calls);
    TEST_ASSERT_EQUAL_INT(1, mk_updi_close_calls);
}

/* LLR-MAIN-04 */
static void test_main_load_flag_writes_all_pt_load_segments_to_flash(void)
{
    uint32_t vmas[]  = { 0x000000u, 0x000200u };
    uint32_t sizes[] = { 8u, 8u };
    mk_elf_tmp_fd = build_min_elf(vmas, sizes, 2, &mk_elf_planted_ehdr);
    mk_elf_have_ehdr = 1;
    mk_elf_planted_sram = 0x800000u;

    char *argv[] = { (char*)"prog", (char*)"--load",
                     (char*)"/dev/x", (char*)"a.elf" };
    TEST_ASSERT_EQUAL_INT(0, app_main(4, argv));
    TEST_ASSERT_EQUAL_INT(2, mk_updi_nvm_calls);
}

/* LLR-MAIN-04 */
static void test_main_load_nvm_write_failure_prints_error_and_returns_1(void)
{
    uint32_t vmas[]  = { 0x000000u };
    uint32_t sizes[] = { 8u };
    mk_elf_tmp_fd = build_min_elf(vmas, sizes, 1, &mk_elf_planted_ehdr);
    mk_elf_have_ehdr = 1;
    mk_elf_planted_sram = 0x800000u;
    mk_updi_nvm_ret = -1;

    char *argv[] = { (char*)"prog", (char*)"--load",
                     (char*)"/dev/x", (char*)"a.elf" };
    TEST_ASSERT_EQUAL_INT(1, app_main(4, argv));
    TEST_ASSERT_EQUAL_INT(1, mk_updi_nvm_calls);
    TEST_ASSERT_EQUAL_INT(0, mk_rsp_listen_calls);
}

/* LLR-MAIN-05 */
static void test_event_loop_uses_single_select_no_pthread_create(void)
{
    AppConfig cfg = { .listen_fd = 4, .gdb_fd = 5, .updi_fd = 6 };
    mk_select_listen_fd = 4; mk_select_gdb_fd = 5; mk_select_updi_fd = 6;
    RspHandlers h; memset(&h, 0, sizeof h);
    event_loop(&cfg, &h);
    TEST_ASSERT_EQUAL_INT(1, mk_select_calls);
    TEST_ASSERT_TRUE(mk_select_listen_in);
    TEST_ASSERT_TRUE(mk_select_gdb_in);
    TEST_ASSERT_TRUE(mk_select_updi_in);
}

/* LLR-MAIN-05 */
static void test_event_loop_accepts_gdb_client_when_gdb_fd_is_minus1(void)
{
    AppConfig cfg = { .listen_fd = 4, .gdb_fd = -1, .updi_fd = 6 };
    mk_select_listen_fd = 4;
    mk_select_ret = 1; mk_select_errno = 0;
    mk_select_force_listen_ready = 1;
    mk_select_set_g_quit = 2; /* set quit on 2nd call so 1st iter processes accept */
    mk_rsp_accept_ret = 99;
    RspHandlers h; memset(&h, 0, sizeof h);
    event_loop(&cfg, &h);
    TEST_ASSERT_EQUAL_INT(99, cfg.gdb_fd);
}

/* LLR-MAIN-06 */
static void test_sigint_handler_sets_g_quit_to_1(void)
{
    g_quit = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    raise(SIGINT);
    TEST_ASSERT_EQUAL_INT(1, (int)g_quit);
}

/* LLR-MAIN-06 */
static void test_event_loop_exits_immediately_when_g_quit_is_1(void)
{
    AppConfig cfg = { .listen_fd = 4, .gdb_fd = -1, .updi_fd = 6 };
    g_quit = 1;
    RspHandlers h; memset(&h, 0, sizeof h);
    event_loop(&cfg, &h);
    TEST_ASSERT_EQUAL_INT(0, mk_select_calls);
}

/* LLR-MAIN-07 */
static void test_main_cleanup_closes_gdb_elf_updi_in_order(void)
{
    uint32_t v[] = {0}, s[] = {0};
    mk_elf_tmp_fd = build_min_elf(v, s, 1, &mk_elf_planted_ehdr);
    mk_elf_have_ehdr = 1;

    char *argv[] = { (char*)"prog", (char*)"/dev/x", (char*)"a.elf" };
    TEST_ASSERT_EQUAL_INT(0, app_main(3, argv));

    int i_rsp = -1, i_elf = -1, i_updi = -1;
    for (int i = 0; i < call_log_len; i++) {
        if (call_log[i] == CALL_RSP_CLOSE && i_rsp == -1) i_rsp = i;
        else if (call_log[i] == CALL_ELF_CLOSE && i_elf == -1) i_elf = i;
        else if (call_log[i] == CALL_UPDI_CLOSE && i_updi == -1) i_updi = i;
    }
    TEST_ASSERT_TRUE(i_rsp >= 0);
    TEST_ASSERT_TRUE(i_elf > i_rsp);
    TEST_ASSERT_TRUE(i_updi > i_elf);
}

/* ── Runner ──────────────────────────────────────────────────────────── */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_args_valid_positional_args_populate_config);
    RUN_TEST(test_parse_args_unrecognised_flag_exits_1);
    RUN_TEST(test_parse_args_default_port_baud_load_when_not_supplied);
    RUN_TEST(test_parse_args_missing_serial_device_exits_1);
    RUN_TEST(test_parse_args_missing_elf_file_exits_1);
    RUN_TEST(test_main_updi_open_failure_releases_resources_and_returns_1);
    RUN_TEST(test_main_rsp_listen_failure_closes_updi_and_returns_1);
    RUN_TEST(test_main_load_flag_writes_all_pt_load_segments_to_flash);
    RUN_TEST(test_main_load_nvm_write_failure_prints_error_and_returns_1);
    RUN_TEST(test_event_loop_uses_single_select_no_pthread_create);
    RUN_TEST(test_event_loop_accepts_gdb_client_when_gdb_fd_is_minus1);
    RUN_TEST(test_sigint_handler_sets_g_quit_to_1);
    RUN_TEST(test_event_loop_exits_immediately_when_g_quit_is_1);
    RUN_TEST(test_main_cleanup_closes_gdb_elf_updi_in_order);
    return UNITY_END();
}
