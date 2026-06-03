/* tests/test_device.c — Unity tests for the --device diagnostic mode
 * (LLR-MAIN-08, LLR-MAIN-09, LLR-UPDI-13 / HLR-044).
 *
 * Strategy:
 *   • #include "main.c" so parse_args / run_device_mode / app_main and
 *     the static device_family[] table are reachable directly.
 *   • Wrap the symbols that would otherwise touch the network / ELF /
 *     FSM layers (rsp_*, elf_*, fsm_*). updi_open and updi_close are
 *     wrapped to substitute a pre-opened PTY slave fd so the real
 *     updi_read_device_info() runs against a scripted PTY harness.
 *   • For the mutual-exclusion test we fork() and inspect captured
 *     stderr + exit status.                                              */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "unity.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __linux__
#include <elf.h>
#else
#include "elf.h"
#endif

#include "elf_parser.h"
#include "updi.h"
#include "fsm_mapper.h"
#include "gdb_rsp.h"

void rsp_set_logging(bool enabled) { (void)enabled; }

/* main.c is brought in so parse_args(), run_device_mode(), app_main(),
 * and the static device_family[] table are visible.                     */
#include "main.c"

/* ── PTY fixture ─────────────────────────────────────────────────────── */
static int  g_master_fd = -1;
static int  g_slave_fd  = -1;
static char g_slave_name[64];

/* ── Mock state ──────────────────────────────────────────────────────── */
static int  mk_updi_open_calls;
static int  mk_updi_open_ret;       /* < 0 forces failure path             */
static int  mk_updi_close_calls;
static int  mk_rsp_listen_calls;
static int  mk_rsp_accept_calls;
static int  mk_elf_open_calls;
static int  mk_fsm_build_calls;

/* ── Wraps ───────────────────────────────────────────────────────────── */
extern int __real_select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                         struct timeval *tv);

int __wrap_select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                  struct timeval *tv);
int __wrap_select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                  struct timeval *tv)
{
    return __real_select(nfds, r, w, e, tv);
}

int __wrap_updi_open(const char *dev, int baud);
int __wrap_updi_open(const char *dev, int baud)
{
    (void)dev; (void)baud;
    mk_updi_open_calls++;
    if (mk_updi_open_ret < 0)
        return -1;
    return g_slave_fd;
}

void __wrap_updi_close(int fd);
void __wrap_updi_close(int fd)
{
    (void)fd;
    mk_updi_close_calls++;
}

/* Phase 9 wraps (LLR-MAIN-16, LLR-MAIN-17): autobaud + fuse pretty-print
 * call into UPDI-layer helpers that we stub here so the --device
 * harness stays deterministic.  updi_probe_baud is hard-coded to
 * "succeed at 115200" so the autobaud line shows up but does not
 * actually drive the bench.  updi_nvm_read returns a hand-crafted
 * FUSE+LOCK image that exercises the decoder paths.                  */
static int      mk_updi_probe_baud_calls;
static int      mk_updi_probe_baud_ret    = 115200;
static int      mk_updi_nvm_read_calls;
static uint8_t  mk_updi_nvm_read_pattern  = 0x00;

int __wrap_updi_probe_baud(const char *dev, int samples,
                           void (*report)(int, int, int, void *),
                           void *user);
int __wrap_updi_probe_baud(const char *dev, int samples,
                           void (*report)(int, int, int, void *),
                           void *user)
{
    (void)dev;
    mk_updi_probe_baud_calls++;
    if (report) report(mk_updi_probe_baud_ret, 0, samples, user);
    return mk_updi_probe_baud_ret;
}

int __wrap_updi_nvm_read(int fd, uint32_t addr, uint8_t *buf, size_t len);
int __wrap_updi_nvm_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)fd; (void)addr;
    mk_updi_nvm_read_calls++;
    memset(buf, mk_updi_nvm_read_pattern, len);
    return 0;
}

int __wrap_rsp_listen(uint16_t port);
int __wrap_rsp_listen(uint16_t port)
{
    (void)port;
    mk_rsp_listen_calls++;
    return -1;
}

int __wrap_rsp_accept(int listen_fd);
int __wrap_rsp_accept(int listen_fd)
{
    (void)listen_fd;
    mk_rsp_accept_calls++;
    return -1;
}

void __wrap_rsp_close(int fd);
void __wrap_rsp_close(int fd) { (void)fd; }

int __wrap_rsp_recv_packet(int fd, char *buf, size_t cap);
int __wrap_rsp_recv_packet(int fd, char *buf, size_t cap)
{
    (void)fd; (void)buf; (void)cap;
    return 0;
}

int __wrap_rsp_dispatch(int fd, const char *packet, RspHandlers *h);
int __wrap_rsp_dispatch(int fd, const char *packet, RspHandlers *h)
{
    (void)fd; (void)packet; (void)h;
    return 0;
}

int __wrap_rsp_dispatch_n(int fd, const char *packet, size_t plen,
                          RspHandlers *h);
int __wrap_rsp_dispatch_n(int fd, const char *packet, size_t plen,
                          RspHandlers *h)
{
    (void)fd; (void)packet; (void)plen; (void)h;
    return 0;
}

void __wrap_rsp_default_handlers(RspHandlers *h, RspContext *ctx);
void __wrap_rsp_default_handlers(RspHandlers *h, RspContext *ctx)
{
    (void)ctx;
    if (h) memset(h, 0, sizeof *h);
}

int __wrap_elf_open(const char *path, ElfContext *ctx);
int __wrap_elf_open(const char *path, ElfContext *ctx)
{
    (void)path;
    mk_elf_open_calls++;
    if (ctx) { memset(ctx, 0, sizeof *ctx); ctx->fd = -1; }
    return 0;
}

void __wrap_elf_close(ElfContext *ctx);
void __wrap_elf_close(ElfContext *ctx) { (void)ctx; }

int __wrap_elf_find_avros_tables(ElfContext *c, AvrOsSymbolIndex *i);
int __wrap_elf_find_avros_tables(ElfContext *c, AvrOsSymbolIndex *i)
{
    (void)c; (void)i;
    return 0;
}

/* elf_has_fsm_symbols is not linked from src/elf_parser.c in this
 * test binary; provide a minimal stub so main.c's reference resolves. */
int elf_has_fsm_symbols(const AvrOsSymbolIndex *idx);
int elf_has_fsm_symbols(const AvrOsSymbolIndex *idx) { (void)idx; return 1; }

int __wrap_fsm_build_thread_list(FsmContext *c, const AvrOsSymbolIndex *i, int fd);
int __wrap_fsm_build_thread_list(FsmContext *c, const AvrOsSymbolIndex *i, int fd)
{
    (void)c; (void)i; (void)fd;
    mk_fsm_build_calls++;
    return 0;
}

int __wrap_updi_nvm_write_flash(int fd, uint32_t a, const uint8_t *d, size_t n);
int __wrap_updi_nvm_write_flash(int fd, uint32_t a, const uint8_t *d, size_t n)
{
    (void)fd; (void)a; (void)d; (void)n;
    return 0;
}

int __wrap_updi_console_poll(int fd, char *buf, size_t cap);
int __wrap_updi_console_poll(int fd, char *buf, size_t cap)
{
    (void)fd; (void)buf; (void)cap;
    return 0;
}

/* ── PTY helpers (same pattern as tests/test_updi.c) ─────────────────── */
static void open_pty_fixture(void)
{
    struct termios tty;
    if (openpty(&g_master_fd, &g_slave_fd, g_slave_name, NULL, NULL) < 0)
        TEST_FAIL_MESSAGE("openpty() failed");

    if (tcgetattr(g_slave_fd, &tty) < 0)
        TEST_FAIL_MESSAGE("tcgetattr(slave) failed");

    tty.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag &= ~(tcflag_t)OPOST;
    tty.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cflag &= ~(tcflag_t)CSIZE;
    tty.c_cflag &= ~(tcflag_t)PARODD;
    tty.c_cflag |= CS8 | CSTOPB | PARENB | CLOCAL | CREAD;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    if (tcsetattr(g_slave_fd, TCSANOW, &tty) < 0)
        TEST_FAIL_MESSAGE("tcsetattr(slave) failed");

    if (fcntl(g_master_fd, F_SETFL, O_NONBLOCK) < 0)
        TEST_FAIL_MESSAGE("fcntl(master, O_NONBLOCK) failed");
}

static void prestuff(int master, const uint8_t *bytes, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(master, bytes + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            TEST_FAIL_MESSAGE("prestuff: write to master failed");
        }
        off += (size_t)w;
    }
}

static void prestuff_fill(int master, uint8_t v, size_t n)
{
    uint8_t pad[64];
    size_t  off = 0;
    memset(pad, v, sizeof(pad));
    while (off < n) {
        size_t chunk = (n - off > sizeof(pad)) ? sizeof(pad) : (n - off);
        prestuff(master, pad, chunk);
        off += chunk;
    }
}

/* mem_read 3-frame setup: ST_PTR_WORD (4 echo + ACK) + REPEAT (3 echo)
 * + LD ptr++ (2 echo). */
static void prestuff_memread_setup(int master)
{
    uint8_t ack = UPDI_ACK;
    prestuff_fill(master, 0x00, 4);          /* ST_PTR_WORD echo */
    prestuff(master, &ack, 1);               /* ACK              */
    prestuff_fill(master, 0x00, 3);          /* REPEAT echo      */
    prestuff_fill(master, 0x00, 2);          /* LD ptr++ echo    */
}

/* LDCS frame: 2 echo bytes + 1 response byte. */
static void prestuff_ldcs(int master, uint8_t resp)
{
    prestuff_fill(master, 0x00, 2);
    prestuff(master, &resp, 1);
}

/* Prestuff a full successful updi_read_device_info() byte stream onto
 * the master end of the PTY:
 *   - SIGROW.DEVICEID0..2 @ 0x1100 (3 bytes signature)
 *   - SYSCFG.REVID        @ 0x0F01 (1 byte; datasheet §8.3.2.1)
 *   - SIGROW.SERNUM0..15  @ 0x1110 (16 bytes; datasheet §7.6.2.3)
 *   - LDCS ASI_SYS_STATUS / ASI_KEY_STATUS / ASI_STATUSB                */
static void prestuff_device_info_success(int master)
{
    static const uint8_t sig[3]    = { 0x1E, 0x97, 0x0A };  /* AVR128DA28 */
    static const uint8_t revid     = 0xA6;
    static const uint8_t serial[16] = {
        0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, 0x70,
        0x81, 0x92, 0xA3, 0xB4, 0xC5, 0xD6, 0xE7, 0xF8
    };

    prestuff_memread_setup(master);
    prestuff(master, sig, 3);

    prestuff_memread_setup(master);
    prestuff(master, &revid, 1);

    prestuff_memread_setup(master);
    prestuff(master, serial, 16);

    prestuff_ldcs(master, 0x82);   /* ASI_SYS_STATUS */
    prestuff_ldcs(master, 0x10);   /* ASI_KEY_STATUS */
    prestuff_ldcs(master, 0x00);   /* ASI_STATUSB    */
}

/* ── Unity hooks ─────────────────────────────────────────────────────── */
void setUp(void)
{
    g_master_fd = -1;
    g_slave_fd  = -1;
    g_slave_name[0] = '\0';
    mk_updi_open_calls = 0;
    mk_updi_open_ret   = 0;
    mk_updi_close_calls = 0;
    mk_rsp_listen_calls = 0;
    mk_rsp_accept_calls = 0;
    mk_elf_open_calls   = 0;
    mk_fsm_build_calls  = 0;
    g_quit = 0;
}

void tearDown(void)
{
    if (g_master_fd >= 0) close(g_master_fd);
    if (g_slave_fd  >= 0) close(g_slave_fd);
    g_master_fd = -1;
    g_slave_fd  = -1;
}

/* ══════════════════════════════════════════════════════════════════════
 *  (a) parse_args accepts --device without <elf-file>     LLR-MAIN-08
 * ════════════════════════════════════════════════════════════════════ */
static void parse_args_accepts_device_flag_without_elf_operand(void)
{
    char *argv[] = { (char*)"avrOSdb",
                     (char*)"--device",
                     (char*)"/dev/ttyUSB0" };
    AppConfig cfg;
    parse_args(3, argv, &cfg);
    TEST_ASSERT_TRUE(cfg.device_info);
    TEST_ASSERT_EQUAL_STRING("/dev/ttyUSB0", cfg.serial_device);
    TEST_ASSERT_NULL(cfg.elf_path);
}

/* ══════════════════════════════════════════════════════════════════════
 *  (b) parse_args rejects --device + --load               LLR-MAIN-08
 * ════════════════════════════════════════════════════════════════════ */
static void parse_args_rejects_device_combined_with_load(void)
{
    int pipefd[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(pipefd));

    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        /* child: redirect stderr into the pipe write end */
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        char *argv[] = { (char*)"avrOSdb",
                         (char*)"--device", (char*)"--load",
                         (char*)"/dev/ttyUSB0", (char*)"fw.elf" };
        AppConfig cfg;
        parse_args(5, argv, &cfg);
        _exit(0);  /* unreached on the rejection path */
    }
    close(pipefd[1]);

    char buf[512];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(1, WEXITSTATUS(status));
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(buf, "--device is mutually exclusive with --load"),
        "expected mutual-exclusion diagnostic on stderr");
}

/* ══════════════════════════════════════════════════════════════════════
 *  (c) updi_read_device_info returns SIGROW + ASI bytes   LLR-UPDI-13
 * ════════════════════════════════════════════════════════════════════ */
static void updi_read_device_info_returns_sigrow_and_asi_bytes(void)
{
    UpdiDeviceInfo info;
    memset(&info, 0xFFu, sizeof info);

    open_pty_fixture();
    prestuff_device_info_success(g_master_fd);

    int rc = updi_read_device_info(g_slave_fd, &info);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NULL(info.fail_op);

    TEST_ASSERT_EQUAL_HEX8(0x1E, info.device_id[0]);
    TEST_ASSERT_EQUAL_HEX8(0x97, info.device_id[1]);
    TEST_ASSERT_EQUAL_HEX8(0x0A, info.device_id[2]);
    TEST_ASSERT_EQUAL_HEX8(0xA6, info.revid);

    static const uint8_t expect_ser[16] = {
        0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, 0x70,
        0x81, 0x92, 0xA3, 0xB4, 0xC5, 0xD6, 0xE7, 0xF8
    };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect_ser, info.serial, 16);

    TEST_ASSERT_EQUAL_HEX8(0x82, info.asi_sys_status);
    TEST_ASSERT_EQUAL_HEX8(0x10, info.asi_key_status);
    TEST_ASSERT_EQUAL_HEX8(0x00, info.asi_statusb);
}

/* ══════════════════════════════════════════════════════════════════════
 *  (d) updi_read_device_info records "sigrow" on failure  LLR-UPDI-13
 * ════════════════════════════════════════════════════════════════════ */
static void updi_read_device_info_reports_failed_step_on_nak(void)
{
    UpdiDeviceInfo info;
    memset(&info, 0, sizeof info);

    open_pty_fixture();
    /* No bytes prestuffed: the SIGROW read times out at the ACK step
     * and updi_mem_read() returns -1.                                   */

    int rc = updi_read_device_info(g_slave_fd, &info);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_NOT_NULL(info.fail_op);
    TEST_ASSERT_EQUAL_STRING("sigrow", info.fail_op);
    TEST_ASSERT_TRUE(info.fail_errno < 0);
}

/* ══════════════════════════════════════════════════════════════════════
 *  (e) run_device_mode prints the report                  LLR-MAIN-09
 * ════════════════════════════════════════════════════════════════════ */
static void run_device_mode_prints_report_to_stdout(void)
{
    open_pty_fixture();
    prestuff_device_info_success(g_master_fd);

    /* Redirect stdout to a temp file. */
    char tmpl[] = "/tmp/aod_devmode_XXXXXX";
    int  out_fd = mkstemp(tmpl);
    TEST_ASSERT_TRUE(out_fd >= 0);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(out_fd, STDOUT_FILENO);

    AppConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.serial_device = g_slave_name;
    cfg.baud_rate     = 115200;
    cfg.device_info   = true;
    cfg.listen_fd = cfg.gdb_fd = cfg.updi_fd = -1;

    int rc = run_device_mode(&cfg);

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, mk_updi_open_calls);
    TEST_ASSERT_EQUAL_INT(1, mk_updi_close_calls);

    /* Read back captured stdout. */
    lseek(out_fd, 0, SEEK_SET);
    char buf[4096];
    ssize_t n = read(out_fd, buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
    close(out_fd);
    unlink(tmpl);

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Serial device:"),
        "report missing 'Serial device:' line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Baud rate:"),
        "report missing 'Baud rate:' line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Signature:"),
        "report missing 'Signature:' line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Family:"),
        "report missing 'Family:' line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Revision:"),
        "report missing 'Revision:' line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Serial:"),
        "report missing 'Serial:' line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "UPDI status:"),
        "report missing 'UPDI status:' line");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "AVR128DA28"),
        "report missing AVR128DA28 family name");
}

/* ══════════════════════════════════════════════════════════════════════
 *  (f) device mode never calls rsp_listen / fsm_*         LLR-MAIN-09
 * ════════════════════════════════════════════════════════════════════ */
static void device_mode_does_not_call_rsp_listen(void)
{
    open_pty_fixture();
    prestuff_device_info_success(g_master_fd);

    /* Silence stdout for this test. */
    fflush(stdout);
    int devnull = open("/dev/null", O_WRONLY);
    int saved   = dup(STDOUT_FILENO);
    if (devnull >= 0) dup2(devnull, STDOUT_FILENO);

    char *argv[] = { (char*)"avrOSdb",
                     (char*)"--device",
                     g_slave_name };
    int rc = app_main(3, argv);

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    if (devnull >= 0) close(devnull);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(0, mk_rsp_listen_calls);
    TEST_ASSERT_EQUAL_INT(0, mk_rsp_accept_calls);
    TEST_ASSERT_EQUAL_INT(0, mk_elf_open_calls);
    TEST_ASSERT_EQUAL_INT(0, mk_fsm_build_calls);
}

/* ── Runner ──────────────────────────────────────────────────────────── */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(parse_args_accepts_device_flag_without_elf_operand);
    RUN_TEST(parse_args_rejects_device_combined_with_load);
    RUN_TEST(updi_read_device_info_returns_sigrow_and_asi_bytes);
    RUN_TEST(updi_read_device_info_reports_failed_step_on_nak);
    RUN_TEST(run_device_mode_prints_report_to_stdout);
    RUN_TEST(device_mode_does_not_call_rsp_listen);
    return UNITY_END();
}
