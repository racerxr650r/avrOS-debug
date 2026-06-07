/* tests/test_updi.c — Phase G (spec-aligned UPDI test suite) */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include "unity.h"
#include "updi.h"

/* Bit-3 of ASI_SYS_STATUS = NVMPROG (datasheet §35.5.9).  Mirrors the
 * private symbol of the same name in src/updi.c. */
#define ASI_SYS_STATUS_NVMPROG_BIT 0x08u

/* ── __wrap_select globals ────────────────────────────────────────────── */

int            g_select_force_timeout = 0;
int            g_last_select_captured = 0;
struct timeval g_last_select_tv;

extern int __real_select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                         struct timeval *tv);

int __wrap_select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                  struct timeval *tv)
{
    if (tv != NULL) {
        g_last_select_tv       = *tv;
        g_last_select_captured = 1;
    }
    if (g_select_force_timeout)
        return 0;
    return __real_select(nfds, r, w, e, tv);
}

/* ── per-test fixture state (auto-cleaned in tearDown) ─────────────────── */

static int  g_master_fd = -1;
static int  g_slave_fd  = -1;
static char g_slave_name[64];

/* ── PTY helpers ──────────────────────────────────────────────────────── */

static void open_pty_pair(int *master_out, int *slave_out, char *slave_name_out)
{
    struct termios tty;

    if (openpty(master_out, slave_out, slave_name_out, NULL, NULL) < 0)
        TEST_FAIL_MESSAGE("openpty() failed");

    if (tcgetattr(*slave_out, &tty) < 0)
        TEST_FAIL_MESSAGE("tcgetattr(slave) failed");

    /* raw 8E2 (matches updi_open) */
    tty.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag &= ~(tcflag_t)OPOST;
    tty.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cflag &= ~(tcflag_t)CSIZE;
    tty.c_cflag &= ~(tcflag_t)PARODD;
    tty.c_cflag |= CS8 | CSTOPB | PARENB | CLOCAL | CREAD;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;   /* 100 ms read timeout */

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

    if (tcsetattr(*slave_out, TCSANOW, &tty) < 0)
        TEST_FAIL_MESSAGE("tcsetattr(slave) failed");

    if (fcntl(*master_out, F_SETFL, O_NONBLOCK) < 0)
        TEST_FAIL_MESSAGE("fcntl(master, O_NONBLOCK) failed");
}

static void close_pty_pair(int master, int slave)
{
    if (master >= 0) close(master);
    if (slave  >= 0) close(slave);
}

static void open_pty_fixture(void)
{
    open_pty_pair(&g_master_fd, &g_slave_fd, g_slave_name);
}

/* Write n bytes to master (into the slave's input queue) */
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

static size_t drain_master(int master, uint8_t *out, size_t cap)
{
    size_t off = 0;
    while (off < cap) {
        ssize_t r = read(master, out + off, cap - off);
        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (r < 0 && errno == EINTR)
            continue;
        break;
    }
    return off;
}

__attribute__((unused))
static void aod_silence_unused(void)
{
    (void)close_pty_pair;
    (void)open_pty_pair;
}

/* Push n filler bytes (value `v`) onto the master. */
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

/*
 * Pre-stuff helpers matching the 3-frame mem_read / mem_write protocol.
 *
 * updi_set_ptr() always emits the 24-bit ST_PTR_LONG form (see the comment
 * on updi_set_ptr in src/updi.c — the 16-bit ST_PTR_WORD form leaves a
 * stale high pointer byte that misdirects SRAM reads into mapped Flash):
 *
 *   ST_PTR_LONG frame  : SYNCH, 0x6A, addr_lo, addr_mid, addr_hi → 5 echo + 1 ACK
 *   REPEAT      frame  : SYNCH, 0xA0, count                      → 3 echo
 *   LD/ST       frame  : SYNCH, 0x24 or 0x64                     → 2 echo
 */
static void prestuff_setptr_ack(int master)
{
    uint8_t ack = UPDI_ACK;
    prestuff_fill(master, 0x00, 5);
    prestuff(master, &ack, 1);
}

/* Alias retained for tests that emphasise the 24-bit pointer-set explicitly. */
static void prestuff_setptr_ack_long(int master)
{
    prestuff_setptr_ack(master);
}

static void prestuff_repeat_echo(int master)   { prestuff_fill(master, 0x00, 3); }
static void prestuff_ldst_echo(int master)     { prestuff_fill(master, 0x00, 2); }

/* ── Unity hooks ──────────────────────────────────────────────────────── */

void setUp(void)
{
    g_select_force_timeout = 0;
    g_last_select_captured = 0;
    memset(&g_last_select_tv, 0, sizeof(g_last_select_tv));
    g_master_fd = -1;
    g_slave_fd  = -1;
    g_slave_name[0] = '\0';
}

void tearDown(void)
{
    close_pty_pair(g_master_fd, g_slave_fd);
    g_master_fd = -1;
    g_slave_fd  = -1;
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Phase B — trivial tests                                               */
/* ══════════════════════════════════════════════════════════════════════ */

/* Test 2 */
static void updi_open_returns_minus1_on_device_open_failure(void)
{
    int rc = updi_open("/dev/nonexistent_aod", 115200);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* Test 13 */
static void updi_nvm_write_flash_rejects_unaligned_address(void)
{
    uint8_t dummy[512] = {0};
    int rc = updi_nvm_write_flash(-1, 1, dummy, 512);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* Test 14 */
static void updi_nvm_write_flash_rejects_non_multiple_of_512_length(void)
{
    uint8_t dummy[100] = {0};
    int rc = updi_nvm_write_flash(-1, 0, dummy, 100);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* Test 23 */
static void updi_console_poll_returns_0_when_output_buffer_empty(void)
{
    char buf[16];
    int  rc;

    open_pty_fixture();
    rc = updi_console_poll(g_slave_fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Phase C — three-frame I/O tests                                       */
/* ══════════════════════════════════════════════════════════════════════ */

/*
 * Test 7: 512-byte read splits into two 256-byte bursts.
 * Each burst emits 10 setup bytes:
 *   ST_PTR_LONG (5) + REPEAT (3) + LD ptr++ (2)
 * Total captured TX setup = 20 bytes; total data RX = 512 bytes.
 */
static void updi_mem_read_splits_request_larger_than_256_bytes(void)
{
    uint8_t  databuf[512];
    uint8_t  captured[32];
    size_t   n;
    int      rc;

    open_pty_fixture();

    /* Block 1 setup + data */
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    prestuff_fill(g_master_fd, 0xAA, 256);
    /* Block 2 setup + data */
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    prestuff_fill(g_master_fd, 0xBB, 256);

    rc = updi_mem_read(g_slave_fd, 0x0100u, databuf, sizeof(databuf));
    TEST_ASSERT_EQUAL_INT(0, rc);

    n = drain_master(g_master_fd, captured, sizeof(captured));
    TEST_ASSERT_EQUAL_size_t(20u, n);

    /* Block 1: ST_PTR_LONG frame → 0x55, 0x6A, 0x00, 0x01, 0x00 */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[0]);
    TEST_ASSERT_EQUAL_HEX8(0x6Au,      captured[1]);   /* ST ptr (long) */
    TEST_ASSERT_EQUAL_HEX8(0x00u,      captured[2]);   /* addr_lo       */
    TEST_ASSERT_EQUAL_HEX8(0x01u,      captured[3]);   /* addr_mid      */
    TEST_ASSERT_EQUAL_HEX8(0x00u,      captured[4]);   /* addr_hi       */
    /* REPEAT frame */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[5]);
    TEST_ASSERT_EQUAL_HEX8(0xA0u,      captured[6]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu,      captured[7]);   /* count = 256-1 */
    /* LD ptr++ frame */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[8]);
    TEST_ASSERT_EQUAL_HEX8(0x24u,      captured[9]);

    /* Block 2: addr advances to 0x0200 */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[10]);
    TEST_ASSERT_EQUAL_HEX8(0x6Au,      captured[11]);
    TEST_ASSERT_EQUAL_HEX8(0x00u,      captured[12]);  /* addr_lo  */
    TEST_ASSERT_EQUAL_HEX8(0x02u,      captured[13]);  /* addr_mid */
    TEST_ASSERT_EQUAL_HEX8(0x00u,      captured[14]);  /* addr_hi  */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[15]);
    TEST_ASSERT_EQUAL_HEX8(0xA0u,      captured[16]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu,      captured[17]);
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[18]);
    TEST_ASSERT_EQUAL_HEX8(0x24u,      captured[19]);
}

/* Test 8: every mem_read block uses ST_PTR_LONG(0x6A) + REPEAT(0xA0) + LD(0x24) */
static void updi_mem_read_uses_repeat_ld_auto_increment_sequence(void)
{
    uint8_t  databuf[16];
    uint8_t  captured[16];
    size_t   n;
    int      rc;

    open_pty_fixture();
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    prestuff_fill(g_master_fd, 0x5Au, 16);

    rc = updi_mem_read(g_slave_fd, 0x0100u, databuf, sizeof(databuf));
    TEST_ASSERT_EQUAL_INT(0, rc);

    n = drain_master(g_master_fd, captured, sizeof(captured));
    TEST_ASSERT_EQUAL_size_t(10u, n);
    TEST_ASSERT_EQUAL_HEX8(0x6Au, captured[1]);    /* ST ptr (long)  */
    TEST_ASSERT_EQUAL_HEX8(0xA0u, captured[6]);    /* REPEAT         */
    TEST_ASSERT_EQUAL_HEX8(0x24u, captured[9]);    /* LD ptr++       */
}

/* Test 8b: mem_read uses 24-bit ST_PTR (0x6A) when addr > 0xFFFF
 * (mapped-Flash region above 64 KiB on AVR128DA/DB). */
static void updi_mem_read_uses_24bit_addressing_above_64kib(void)
{
    uint8_t  databuf[4];
    uint8_t  captured[16];
    size_t   n;
    int      rc;

    open_pty_fixture();
    prestuff_setptr_ack_long(g_master_fd);   /* 5 echo + 1 ACK */
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    prestuff_fill(g_master_fd, 0x7Eu, 4);

    /* Target: 0x012345 = lo 0x45, mid 0x23, hi 0x01 */
    rc = updi_mem_read(g_slave_fd, 0x012345u, databuf, sizeof(databuf));
    TEST_ASSERT_EQUAL_INT(0, rc);

    n = drain_master(g_master_fd, captured, sizeof(captured));
    TEST_ASSERT_EQUAL_size_t(10u, n);   /* 5 + 3 + 2 */

    /* ST_PTR_LONG frame: SYNCH, 0x6A, 0x45, 0x23, 0x01 */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[0]);
    TEST_ASSERT_EQUAL_HEX8(0x6Au,      captured[1]);
    TEST_ASSERT_EQUAL_HEX8(0x45u,      captured[2]);
    TEST_ASSERT_EQUAL_HEX8(0x23u,      captured[3]);
    TEST_ASSERT_EQUAL_HEX8(0x01u,      captured[4]);
    /* REPEAT + LD ptr++ */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[5]);
    TEST_ASSERT_EQUAL_HEX8(0xA0u,      captured[6]);
    TEST_ASSERT_EQUAL_HEX8(0x03u,      captured[7]);   /* count - 1 = 3 */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[8]);
    TEST_ASSERT_EQUAL_HEX8(0x24u,      captured[9]);

    TEST_ASSERT_EQUAL_HEX8(0x7Eu, databuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7Eu, databuf[3]);
}

/* Test 9: mem_read passes a 0s + 100000us timeval to select() */static void updi_mem_read_calls_select_with_100ms_timeout_before_read(void)
{
    uint8_t  databuf[16];
    int      rc;

    open_pty_fixture();
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    prestuff_fill(g_master_fd, 0xC3u, 16);

    rc = updi_mem_read(g_slave_fd, 0x0100u, databuf, sizeof(databuf));
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, g_last_select_captured);
    TEST_ASSERT_EQUAL_INT(0,      (int)g_last_select_tv.tv_sec);
    TEST_ASSERT_EQUAL_INT(100000, (int)g_last_select_tv.tv_usec);
}

/* Test 10: mem_read returns -1 immediately when select() times out */
static void updi_mem_read_returns_minus1_on_select_timeout(void)
{
    uint8_t databuf[16];
    int     rc;

    open_pty_fixture();
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);

    g_select_force_timeout = 1;
    rc = updi_mem_read(g_slave_fd, 0x0100u, databuf, sizeof(databuf));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* Test 11: mem_write returns 0 when each data byte is ACKed */
static void updi_mem_write_returns_0_on_success(void)
{
    static const uint8_t payload[2] = { 0x12, 0x34 };
    uint8_t  ack = UPDI_ACK;
    int      rc;

    open_pty_fixture();
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    /* byte 0: 1 echo + ACK */
    prestuff_fill(g_master_fd, 0x00, 1);
    prestuff(g_master_fd, &ack, 1);
    /* byte 1: 1 echo + ACK */
    prestuff_fill(g_master_fd, 0x00, 1);
    prestuff(g_master_fd, &ack, 1);

    rc = updi_mem_write(g_slave_fd, 0x0100u, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/* Test 12: mem_write returns -1 when target sends a NAK instead of ACK */
static void updi_mem_write_returns_minus1_on_updi_nak(void)
{
    static const uint8_t payload[1] = { 0x42 };
    uint8_t  nak = 0x00;
    int      rc;

    open_pty_fixture();
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    prestuff_fill(g_master_fd, 0x00, 1);     /* data echo */
    prestuff(g_master_fd, &nak, 1);          /* NAK      */

    rc = updi_mem_write(g_slave_fd, 0x0100u, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* Test 17/18/19: halt/run/step now drive the AVR-Dx OCD STOP/RUN/STEP
 * primitives.  With a bare PTY (no scripted echo), the LDCS poll for
 * STOPPED never satisfies, so each call must return -1.  This proves
 * the OCD-aware error path is reachable and bounded.                */
static void updi_halt_returns_minus1_when_target_never_acknowledges_stopped(void)
{
    open_pty_fixture();
    TEST_ASSERT_EQUAL_INT(-1, updi_halt(g_slave_fd));
}
static void updi_run_returns_minus1_on_stcs_link_failure(void)
{
    open_pty_fixture();
    /* Close master so STCS has nowhere to land → -1. */
    close(g_master_fd); g_master_fd = -1;
    TEST_ASSERT_EQUAL_INT(-1, updi_run(g_slave_fd));
}
static void updi_step_returns_minus1_when_target_never_acknowledges_stopped(void)
{
    open_pty_fixture();
    TEST_ASSERT_EQUAL_INT(-1, updi_step(g_slave_fd));
}

/* Phase 20: debug-in-sleep. When enabled (the default), applying the policy on
 * a STOPPED target asserts CLK_REQ via STCS ASI_SYS_CTRLA — bytes
 * { SYNCH=0x55, STCS|0x0A=0xCA, CLK_REQ=0x01 }.  updi_write_bytes() drains its
 * own 3-byte half-duplex echo, so the echo is prestuffed. */
static void updi_apply_debug_in_sleep_asserts_clkreq_when_enabled(void)
{
    static const uint8_t echo[3] = { 0x55u, 0xCAu, 0x01u };
    uint8_t got[8];
    size_t  n;

    open_pty_fixture();
    updi_set_debug_in_sleep(true);
    prestuff(g_master_fd, echo, sizeof echo);     /* satisfy the echo read */

    TEST_ASSERT_EQUAL_INT(0, updi_apply_debug_in_sleep(g_slave_fd));

    n = drain_master(g_master_fd, got, sizeof got);
    TEST_ASSERT_EQUAL_size_t(3u, n);
    TEST_ASSERT_EQUAL_HEX8(0x55u, got[0]);        /* SYNCH                    */
    TEST_ASSERT_EQUAL_HEX8(0xCAu, got[1]);        /* STCS | ASI_SYS_CTRLA     */
    TEST_ASSERT_EQUAL_HEX8(0x01u, got[2]);        /* CLK_REQ                  */
}

/* When disabled (--sleep), applying the policy is a no-op — nothing is sent,
 * so native target sleep behaviour is left intact. */
static void updi_apply_debug_in_sleep_emits_nothing_when_disabled(void)
{
    uint8_t got[8];
    size_t  n;

    open_pty_fixture();
    updi_set_debug_in_sleep(false);

    TEST_ASSERT_EQUAL_INT(0, updi_apply_debug_in_sleep(g_slave_fd));

    n = drain_master(g_master_fd, got, sizeof got);
    TEST_ASSERT_EQUAL_size_t(0u, n);

    updi_set_debug_in_sleep(true);                /* restore default          */
}

/* Test 22: console_poll returns pending bytes without halting */
static void updi_console_poll_returns_pending_bytes_without_halting(void)
{
    static const uint8_t payload[4] = { 'A', 'B', 'C', 'D' };
    char     buf[16];
    uint8_t  captured[8];
    size_t   n;
    int      rc;

    open_pty_fixture();
    prestuff(g_master_fd, payload, sizeof(payload));

    rc = updi_console_poll(g_slave_fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(4, rc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, buf, 4);

    n = drain_master(g_master_fd, captured, sizeof(captured));
    TEST_ASSERT_EQUAL_size_t(0u, n);
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Phase D — NVM tests                                                   */
/* ══════════════════════════════════════════════════════════════════════ */

/*
 * Test 15: KEY accepted, two reset STCS frames accepted, but
 * ASI_SYS_STATUS.NVMPROG (bit 3 = 0x08) never asserts ⇒ -1 after 100 polls.
 *
 * Per attempt: KEY = 10 echo bytes; each STCS = 3 echo bytes (no ACK).
 * Then 100 LDCS polls: 2 echo + 1 status byte each.
 */
static void updi_nvm_write_flash_nvmprog_poll_timeout_returns_minus1(void)
{
    uint8_t  data[512];
    uint8_t  zero = 0x00;
    int      i;
    int      rc;

    open_pty_fixture();
    memset(data, 0x5A, sizeof(data));

    prestuff_fill(g_master_fd, 0x00, 10);    /* KEY echo                 */
    prestuff_fill(g_master_fd, 0x00, 3);     /* STCS reset 0x59          */
    prestuff_fill(g_master_fd, 0x00, 3);     /* STCS reset 0x00          */

    for (i = 0; i < 100; i++) {
        prestuff_fill(g_master_fd, 0x00, 2); /* LDCS echo                */
        prestuff(g_master_fd, &zero, 1);     /* NVMPROG bit always 0     */
    }

    rc = updi_nvm_write_flash(g_slave_fd, 0u, data, sizeof(data));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/*
 * Test 16: NVMPROG asserts on the first poll, the page programs, but the
 * NVMCTRL BUSY bit never clears ⇒ -1 after 20 polls.
 *
 * After entry reset writes + LDCS:
 *   - mem_write of 512-byte page:
 *       ST_PTR (5: 4 echo + ACK) + REPEAT (3 echo) + ST ptr++ (2 echo)
 *       + 512 × (1 echo + 1 ACK)
 *   - mem_write of NVMCTRL_CTRLA = 0x03 (1 byte to addr 0x1000):
 *       ST_PTR + REPEAT + ST ptr++ + 1 × (1 echo + 1 ACK)
 *   - 20 BUSY polls via mem_read(0x1002, 1 byte):
 *       ST_PTR + REPEAT + LD ptr++ + 1 status byte each
 */
static void updi_nvm_write_flash_per_page_busy_timeout_returns_minus1(void)
{
    uint8_t  data[512];
    uint8_t  nvmprog_set = ASI_SYS_STATUS_NVMPROG_BIT;
    uint8_t  ack         = UPDI_ACK;
    uint8_t  busy        = 0x01;
    int      i;
    int      rc;

    open_pty_fixture();
    memset(data, 0xA5, sizeof(data));

    /* 1) KEY + 2 reset STCS frames */
    prestuff_fill(g_master_fd, 0x00, 10);
    prestuff_fill(g_master_fd, 0x00, 3);
    prestuff_fill(g_master_fd, 0x00, 3);

    /* 2) LDCS NVMPROG poll, ready on first try */
    prestuff_fill(g_master_fd, 0x00, 2);
    prestuff(g_master_fd, &nvmprog_set, 1);

    /* 3) mem_write of 512-byte page */
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    for (i = 0; i < 512; i++) {
        prestuff_fill(g_master_fd, 0x00, 1);
        prestuff(g_master_fd, &ack, 1);
    }

    /* 4) mem_write of NVMCTRL_CTRLA = 0x03 (1 byte) */
    prestuff_setptr_ack(g_master_fd);
    prestuff_repeat_echo(g_master_fd);
    prestuff_ldst_echo(g_master_fd);
    prestuff_fill(g_master_fd, 0x00, 1);
    prestuff(g_master_fd, &ack, 1);

    /* 5) 20 BUSY polls via mem_read */
    for (i = 0; i < 20; i++) {
        prestuff_setptr_ack(g_master_fd);
        prestuff_repeat_echo(g_master_fd);
        prestuff_ldst_echo(g_master_fd);
        prestuff(g_master_fd, &busy, 1);
    }

    rc = updi_nvm_write_flash(g_slave_fd, 0u, data, sizeof(data));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Phase E — updi_open tests (scripted responder thread)                 */
/* ══════════════════════════════════════════════════════════════════════ */

/*
 * Per-attempt SUT byte stream (9 bytes) — cold-start handshake (issue #19):
 *   0: 0x00         wake byte (tcflush absorbs the echo, so inject nothing)
 *   1: 0x55         STCS CTRLB frame SYNCH        → 1 echo byte
 *   2: 0xC3         STCS CTRLB opcode             → 1 echo byte
 *   3: 0x08         STCS CTRLB value (CCDETDIS)   → 1 echo byte
 *   4: 0x55         STCS CTRLA frame SYNCH        → 1 echo byte
 *   5: 0xC2         STCS CTRLA opcode             → 1 echo byte
 *   6: 0x80         STCS CTRLA value (IBDLY)      → 1 echo byte
 *   7: 0x55         LDCS STATUSA frame SYNCH      → 1 echo byte
 *   8: 0x80         LDCS STATUSA opcode           → 1 echo byte + 1 STATUSA byte
 */
typedef struct {
    int             master_fd;
    pthread_mutex_t mtx;
    int             stop;
    uint8_t         capture[64];
    size_t          n_captured;
} responder_t;

static void *responder_thread(void *arg)
{
    responder_t *r = (responder_t *)arg;
    static const uint8_t echo_byte    = 0xAA;
    static const uint8_t ldcs_resp[2] = { 0xAA, 0x30 };  /* echo + UPDIREV */
    /* SIB response: 1 echo byte for the 0xE6 opcode + 32 bytes of SIB. */
    static const uint8_t sib_resp[1 + 32] = {
        0xAA,
        ' ', ' ', ' ', ' ', 'A', 'V', 'R', ' ',
        'P', ':', '2', 'D', ':', '1', '-', '3',
        'M', '2', ' ', '(', 'A', '7', '.', 'K',
        'V', '0', '0', '1', '.', '0', ')', '\0'
    };

    while (1) {
        fd_set         rfds;
        struct timeval tv = { 0, 20000 };
        int            sel;
        uint8_t        b;
        ssize_t        n;
        size_t         pos;

        pthread_mutex_lock(&r->mtx);
        if (r->stop) {
            pthread_mutex_unlock(&r->mtx);
            break;
        }
        pthread_mutex_unlock(&r->mtx);

        FD_ZERO(&rfds);
        FD_SET(r->master_fd, &rfds);
        sel = __real_select(r->master_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0)
            continue;

        n = read(r->master_fd, &b, 1);
        if (n != 1)
            continue;

        pthread_mutex_lock(&r->mtx);
        if (r->n_captured < sizeof(r->capture))
            r->capture[r->n_captured++] = b;
        pos = r->n_captured;
        pthread_mutex_unlock(&r->mtx);

        switch ((pos - 1u) % 11u) {
            case 0u: /* wake byte — tcflush absorbs anything we send */    break;
            case 1u: /* STCS CTRLB SYNCH echo */
            case 2u: /* STCS CTRLB opcode echo */
            case 3u: /* STCS CTRLB value echo */
            case 4u: /* STCS CTRLA SYNCH echo */
            case 5u: /* STCS CTRLA opcode echo */
            case 6u: /* STCS CTRLA value echo */
            case 7u: /* LDCS STATUSA SYNCH echo */
            case 9u: /* SIB SYNCH echo */
                { ssize_t w = write(r->master_fd, &echo_byte, 1); (void)w; }
                break;
            case 8u: /* LDCS STATUSA opcode echo + UPDIREV response */
                { ssize_t w = write(r->master_fd, ldcs_resp, sizeof(ldcs_resp)); (void)w; }
                break;
            case 10u: /* SIB opcode echo + 16-byte SIB payload */
                { ssize_t w = write(r->master_fd, sib_resp, sizeof(sib_resp)); (void)w; }
                break;
        }
    }
    return NULL;
}

static pthread_t responder_start(responder_t *r, int master_fd)
{
    pthread_t tid;
    pthread_mutex_init(&r->mtx, NULL);
    r->master_fd  = master_fd;
    r->stop       = 0;
    r->n_captured = 0;
    if (pthread_create(&tid, NULL, responder_thread, r) != 0)
        TEST_FAIL_MESSAGE("pthread_create failed");
    return tid;
}

static void responder_stop(pthread_t tid, responder_t *r)
{
    pthread_mutex_lock(&r->mtx);
    r->stop = 1;
    pthread_mutex_unlock(&r->mtx);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&r->mtx);
}

/* Test 1: termios is configured for 8E2 (even parity, 2 stop bits) raw.
 *
 * Note: Linux PTY slaves silently strip PARENB on open-by-name, so even
 * though updi_open requests 8E2, the readback under tests will report
 * PARENB clear.  We therefore verify only the bits the PTY does honor
 * (CS8, CSTOPB, raw input/output/local flags).  PARENB is exercised on
 * real hardware via integration tests in Phase 3.
 */
static void updi_open_sets_8e2_raw_half_duplex_via_termios(void)
{
    responder_t    rsp;
    pthread_t      tid;
    struct termios tty;
    int            sut_fd;

    open_pty_fixture();
    tid    = responder_start(&rsp, g_master_fd);
    sut_fd = updi_open(g_slave_name, 115200);
    responder_stop(tid, &rsp);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sut_fd);
    TEST_ASSERT_EQUAL_INT(0, tcgetattr(sut_fd, &tty));

    TEST_ASSERT_EQUAL_HEX((tcflag_t)CS8, tty.c_cflag & (tcflag_t)CSIZE);
    TEST_ASSERT_TRUE_MESSAGE((tty.c_cflag & (tcflag_t)CSTOPB) != 0,
                             "CSTOPB (2 stop bits) must be set");
    TEST_ASSERT_TRUE_MESSAGE((tty.c_cflag & (tcflag_t)PARODD) == 0,
                             "PARODD must be clear (even parity, when supported)");
    TEST_ASSERT_TRUE((tty.c_lflag & (tcflag_t)ICANON) == 0);
    TEST_ASSERT_TRUE((tty.c_lflag & (tcflag_t)ECHO)   == 0);
    TEST_ASSERT_TRUE((tty.c_lflag & (tcflag_t)ISIG)   == 0);
    TEST_ASSERT_TRUE((tty.c_oflag & (tcflag_t)OPOST)  == 0);

    close(sut_fd);
}

/* Test 3: wake byte precedes the STCS CTRLB frame (cold-start, issue #19). */
static void updi_open_asserts_wake_byte_then_stcs_ctrlb(void)
{
    responder_t rsp;
    pthread_t   tid;
    int         sut_fd;
    uint8_t     cap0, cap1, cap2, cap3;

    open_pty_fixture();
    tid    = responder_start(&rsp, g_master_fd);
    sut_fd = updi_open(g_slave_name, 115200);
    responder_stop(tid, &rsp);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sut_fd);
    pthread_mutex_lock(&rsp.mtx);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(4u, rsp.n_captured);
    cap0 = rsp.capture[0];
    cap1 = rsp.capture[1];
    cap2 = rsp.capture[2];
    cap3 = rsp.capture[3];
    pthread_mutex_unlock(&rsp.mtx);

    TEST_ASSERT_EQUAL_HEX8(0x00u,                              cap0);
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH,                         cap1);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(0xC0u | ASI_CTRLB),       cap2);
    TEST_ASSERT_EQUAL_HEX8(ASI_CTRLB_CCDETDIS,                 cap3);

    close(sut_fd);
}

/* Test 3b: SUT then writes STCS CTRLA=IBDLY and probes LDCS STATUSA. */
static void updi_open_issues_stcs_ctrla_and_ldcs_statusa(void)
{
    responder_t rsp;
    pthread_t   tid;
    int         sut_fd;
    uint8_t     stcs_a_sync, stcs_a_op, stcs_a_val;
    uint8_t     ldcs_sync,   ldcs_op;

    open_pty_fixture();
    tid    = responder_start(&rsp, g_master_fd);
    sut_fd = updi_open(g_slave_name, 115200);
    responder_stop(tid, &rsp);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sut_fd);
    pthread_mutex_lock(&rsp.mtx);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(9u, rsp.n_captured);
    stcs_a_sync = rsp.capture[4];
    stcs_a_op   = rsp.capture[5];
    stcs_a_val  = rsp.capture[6];
    ldcs_sync   = rsp.capture[7];
    ldcs_op     = rsp.capture[8];
    pthread_mutex_unlock(&rsp.mtx);

    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH,                        stcs_a_sync);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(0xC0u | ASI_CTRLA),      stcs_a_op);
    TEST_ASSERT_EQUAL_HEX8(ASI_CTRLA_IBDLY,                   stcs_a_val);
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH,                        ldcs_sync);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x80u | ASI_STATUSA),    ldcs_op);

    close(sut_fd);
}

/* Test 4: after the cold-start, baud remains the configured session rate. */
static void updi_open_restores_session_baud_after_break(void)
{
    responder_t    rsp;
    pthread_t      tid;
    struct termios tty;
    int            sut_fd;

    open_pty_fixture();
    tid    = responder_start(&rsp, g_master_fd);
    sut_fd = updi_open(g_slave_name, 115200);
    responder_stop(tid, &rsp);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sut_fd);
    TEST_ASSERT_EQUAL_INT(0, tcgetattr(sut_fd, &tty));
    TEST_ASSERT_EQUAL_UINT(B115200, cfgetispeed(&tty));
    TEST_ASSERT_EQUAL_UINT(B115200, cfgetospeed(&tty));

    close(sut_fd);
}

/* Test 5: with no responder, the cold-start sequence is retried 3 times. */
static void updi_open_retries_cold_start_3_times_on_no_ack(void)
{
    uint8_t cap[64];
    size_t  n, i, wake_bytes;
    int     sut_fd;

    open_pty_fixture();
    sut_fd = updi_open(g_slave_name, 115200);

    TEST_ASSERT_EQUAL_INT(-1, sut_fd);

    n = drain_master(g_master_fd, cap, sizeof(cap));
    /* Cold-start attempt counting (matches avrdude's serialupdi):
     *   attempt 0 (fast path): 1× 0x00 wake byte, then STCS+STCS+LDCS
     *   attempt 1 (slow path): 2× 0x00 break bytes at 300 baud, then STCS+STCS+LDCS
     *   attempt 2 (slow path): 2× 0x00 break bytes at 300 baud, then STCS+STCS+LDCS
     * No other byte in any frame is 0x00, so counting zeros gives 1+2+2 = 5. */
    wake_bytes = 0;
    for (i = 0; i < n; i++)
        if (cap[i] == 0x00u)
            wake_bytes++;
    TEST_ASSERT_EQUAL_size_t(5u, wake_bytes);
}

/* Test 6: after 3 failed BREAK+SYNCH attempts, updi_open returns -1. */
static void updi_open_returns_minus1_after_3_consecutive_link_failures(void)
{
    int sut_fd;

    open_pty_fixture();
    sut_fd = updi_open(g_slave_name, 115200);
    TEST_ASSERT_EQUAL_INT(-1, sut_fd);
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Phase 8 — non-FLASH NVM programming (LLR-UPDI-16..19)                 */
/* ══════════════════════════════════════════════════════════════════════ */

/* LLR-UPDI-16: EEPROM writer rejects an address below UPDI_EEPROM_BASE
 * without issuing any UPDI traffic. */
static void updi_nvm_write_eeprom_rejects_addr_below_window(void)
{
    uint8_t data[1] = { 0xA5 };
    open_pty_fixture();
    int rc = updi_nvm_write_eeprom(g_slave_fd, UPDI_EEPROM_BASE - 1u,
                                   data, sizeof data);
    TEST_ASSERT_EQUAL_INT(-1, rc);

    /* No bytes should have been transmitted. */
    uint8_t cap[16];
    size_t n = drain_master(g_master_fd, cap, sizeof cap);
    TEST_ASSERT_EQUAL_size_t(0u, n);
}

/* LLR-UPDI-16: EEPROM writer rejects (addr+len) past UPDI_EEPROM_SIZE. */
static void updi_nvm_write_eeprom_rejects_addr_above_window(void)
{
    uint8_t data[2] = { 0xA5, 0x5A };
    open_pty_fixture();
    int rc = updi_nvm_write_eeprom(
        g_slave_fd, UPDI_EEPROM_BASE + UPDI_EEPROM_SIZE - 1u,
        data, sizeof data);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* LLR-UPDI-17: FUSES writer rejects len that exceeds the 32-byte window. */
static void updi_nvm_write_fuses_rejects_len_exceeds_window(void)
{
    uint8_t data[UPDI_FUSES_SIZE + 1];
    memset(data, 0xFF, sizeof data);
    open_pty_fixture();
    int rc = updi_nvm_write_fuses(g_slave_fd, UPDI_FUSES_BASE,
                                  data, sizeof data);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* LLR-UPDI-18: USERROW writer rejects zero-length write. */
static void updi_nvm_write_userrow_rejects_zero_length(void)
{
    open_pty_fixture();
    int rc = updi_nvm_write_userrow(g_slave_fd, UPDI_USERROW_BASE, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* LLR-UPDI-19: lockbits writer returns UPDI_ERR_LOCKED when LOCKSTATUS
 * (ASI_SYS_STATUS bit 1) is asserted — even with the unlock pattern. */
static void updi_nvm_write_lockbits_returns_locked_when_lockstatus_set(void)
{
    /* LDCS ASI_SYS_STATUS = 0x02 → LOCKSTATUS asserted.  Each LDCS frame
     * is SYNCH + opcode (2 echo bytes) followed by 1 status byte. */
    uint8_t  lockstatus_set = 0x02;
    uint8_t  data[4] = { 0x5C, 0xC5, 0xC5, 0x5C };  /* unlock pattern */

    open_pty_fixture();
    prestuff_fill(g_master_fd, 0x00, 2);
    prestuff(g_master_fd, &lockstatus_set, 1);

    int rc = updi_nvm_write_lockbits(g_slave_fd, UPDI_LOCK_BASE,
                                     data, sizeof data, true);
    TEST_ASSERT_EQUAL_INT(UPDI_ERR_LOCKED, rc);
}

/* LLR-UPDI-19: lockbits writer returns UPDI_ERR_LOCKED when the payload
 * is NOT the UPDI-unlock pattern and allow_updi_disable=false. */
static void updi_nvm_write_lockbits_refuses_updidis_value_without_flag(void)
{
    /* LDCS ASI_SYS_STATUS = 0x00 → post-erase / unlocked. */
    uint8_t  lockstatus_clr = 0x00;
    uint8_t  data[4] = { 0x00, 0x00, 0x00, 0x00 };  /* would disable UPDI */

    open_pty_fixture();
    prestuff_fill(g_master_fd, 0x00, 2);
    prestuff(g_master_fd, &lockstatus_clr, 1);

    int rc = updi_nvm_write_lockbits(g_slave_fd, UPDI_LOCK_BASE,
                                     data, sizeof data, false);
    TEST_ASSERT_EQUAL_INT(UPDI_ERR_LOCKED, rc);
}

/* LLR-UPDI-27: updi_get_device() defaults to AVR-DA before any
 * updi_select_device() call (preserves historical compile-time defaults). */
static void updi_get_device_default_is_avrda(void)
{
    const UpdiDeviceMap *dev = updi_get_device();
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_STRING("AVR-DA", dev->family);
    TEST_ASSERT_EQUAL_HEX32(UPDI_USERROW_BASE, dev->userrow_base);
    TEST_ASSERT_EQUAL_UINT32(UPDI_USERROW_SIZE, dev->userrow_size);
    TEST_ASSERT_EQUAL_HEX32(UPDI_EEPROM_BASE,  dev->eeprom_base);
    TEST_ASSERT_EQUAL_UINT32(UPDI_EEPROM_SIZE, dev->eeprom_size);
    TEST_ASSERT_TRUE(dev->hw_tested);
}

/* LLR-UPDI-27: --force-device=<name> selects the matching table entry
 * case-insensitively without any UPDI traffic. */
static void updi_select_device_force_avrdd_sets_active_descriptor(void)
{
    /* Use fd=-1: forced path must not touch the wire. */
    int rc = updi_select_device(-1, "avr-dd");
    TEST_ASSERT_EQUAL_INT(0, rc);
    const UpdiDeviceMap *dev = updi_get_device();
    TEST_ASSERT_EQUAL_STRING("AVR-DD", dev->family);
    TEST_ASSERT_EQUAL_UINT32(128u, dev->userrow_size);
    TEST_ASSERT_FALSE(dev->hw_tested);

    /* Restore AVR-DA default for subsequent tests. */
    (void)updi_select_device(-1, "AVR-DA");
}

/* LLR-UPDI-27: --force-device with an unknown family name returns -1. */
static void updi_select_device_unknown_family_returns_minus1(void)
{
    int rc = updi_select_device(-1, "AVR-XYZ");
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* LLR-UPDI-28: updi_family_from_partname() table-driven mapping.
 * Covers every prefix in g_partname_prefix[] plus rejection cases. */
static void updi_family_from_partname_maps_all_known_prefixes(void)
{
    TEST_ASSERT_EQUAL_STRING("AVR-DA",
        updi_family_from_partname("avr128da28"));
    TEST_ASSERT_EQUAL_STRING("AVR-DB",
        updi_family_from_partname("avr128db48"));
    TEST_ASSERT_EQUAL_STRING("AVR-DD",
        updi_family_from_partname("avr64dd32"));
    TEST_ASSERT_EQUAL_STRING("AVR-DU",
        updi_family_from_partname("avr32du28"));
    TEST_ASSERT_EQUAL_STRING("AVR-SD",
        updi_family_from_partname("avr32sd20"));
}

static void updi_family_from_partname_rejects_invalid_inputs(void)
{
    TEST_ASSERT_NULL(updi_family_from_partname(NULL));
    TEST_ASSERT_NULL(updi_family_from_partname(""));
    TEST_ASSERT_NULL(updi_family_from_partname("not-an-avr"));
    /* Unknown family letters → NULL (so main.c falls back to autodetect). */
    TEST_ASSERT_NULL(updi_family_from_partname("avr32ea48"));
    TEST_ASSERT_NULL(updi_family_from_partname("avr16eb14"));
    /* Missing digits between "avr" and letters → NULL. */
    TEST_ASSERT_NULL(updi_family_from_partname("avrda28"));
}

/* ── Phase 9 — updi_crc32() IEEE 802.3 reference vectors (LLR-UPDI-30) ── */

static void updi_crc32_returns_zero_for_empty_buffer(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, updi_crc32(NULL, 0));
}

static void updi_crc32_matches_known_vector_for_123456789(void)
{
    /* IEEE 802.3 / zlib canonical test vector. */
    const unsigned char v[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, updi_crc32(v, 9));
}

static void updi_crc32_matches_known_vector_for_single_zero_byte(void)
{
    const unsigned char z[1] = { 0 };
    TEST_ASSERT_EQUAL_HEX32(0xD202EF8Du, updi_crc32(z, 1));
}

/* ── Test runner ─────────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    /* Phase B */
    RUN_TEST(updi_open_returns_minus1_on_device_open_failure);
    RUN_TEST(updi_nvm_write_flash_rejects_unaligned_address);
    RUN_TEST(updi_nvm_write_flash_rejects_non_multiple_of_512_length);
    RUN_TEST(updi_console_poll_returns_0_when_output_buffer_empty);

    /* Phase C */
    RUN_TEST(updi_mem_read_splits_request_larger_than_256_bytes);
    RUN_TEST(updi_mem_read_uses_repeat_ld_auto_increment_sequence);
    RUN_TEST(updi_mem_read_uses_24bit_addressing_above_64kib);
    RUN_TEST(updi_mem_read_calls_select_with_100ms_timeout_before_read);
    RUN_TEST(updi_mem_read_returns_minus1_on_select_timeout);
    RUN_TEST(updi_mem_write_returns_0_on_success);
    RUN_TEST(updi_mem_write_returns_minus1_on_updi_nak);
    RUN_TEST(updi_halt_returns_minus1_when_target_never_acknowledges_stopped);
    RUN_TEST(updi_run_returns_minus1_on_stcs_link_failure);
    RUN_TEST(updi_step_returns_minus1_when_target_never_acknowledges_stopped);
    RUN_TEST(updi_apply_debug_in_sleep_asserts_clkreq_when_enabled);
    RUN_TEST(updi_apply_debug_in_sleep_emits_nothing_when_disabled);
    RUN_TEST(updi_console_poll_returns_pending_bytes_without_halting);

    /* Phase D */
    RUN_TEST(updi_nvm_write_flash_nvmprog_poll_timeout_returns_minus1);
    RUN_TEST(updi_nvm_write_flash_per_page_busy_timeout_returns_minus1);

    /* Phase E */
    RUN_TEST(updi_open_sets_8e2_raw_half_duplex_via_termios);
    RUN_TEST(updi_open_asserts_wake_byte_then_stcs_ctrlb);
    RUN_TEST(updi_open_issues_stcs_ctrla_and_ldcs_statusa);
    RUN_TEST(updi_open_restores_session_baud_after_break);
    RUN_TEST(updi_open_retries_cold_start_3_times_on_no_ack);
    RUN_TEST(updi_open_returns_minus1_after_3_consecutive_link_failures);

    /* Phase 8 — non-FLASH NVM */
    RUN_TEST(updi_nvm_write_eeprom_rejects_addr_below_window);
    RUN_TEST(updi_nvm_write_eeprom_rejects_addr_above_window);
    RUN_TEST(updi_nvm_write_fuses_rejects_len_exceeds_window);
    RUN_TEST(updi_nvm_write_userrow_rejects_zero_length);
    RUN_TEST(updi_nvm_write_lockbits_returns_locked_when_lockstatus_set);
    RUN_TEST(updi_nvm_write_lockbits_refuses_updidis_value_without_flag);

    /* Phase 9 — multi-family runtime device dispatch (LLR-UPDI-27) */
    RUN_TEST(updi_get_device_default_is_avrda);
    RUN_TEST(updi_select_device_force_avrdd_sets_active_descriptor);
    RUN_TEST(updi_select_device_unknown_family_returns_minus1);
    RUN_TEST(updi_family_from_partname_maps_all_known_prefixes);
    RUN_TEST(updi_family_from_partname_rejects_invalid_inputs);

    /* Phase 9 — CRC32 reference vectors (LLR-UPDI-30) */
    RUN_TEST(updi_crc32_returns_zero_for_empty_buffer);
    RUN_TEST(updi_crc32_matches_known_vector_for_123456789);
    RUN_TEST(updi_crc32_matches_known_vector_for_single_zero_byte);

    return UNITY_END();
}
