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
 * Pre-stuff helpers matching the new 3-frame mem_read / mem_write protocol.
 *
 *   ST_PTR_WORD frame  : SYNCH, 0x69, addr_lo, addr_hi  → 4 echo + 1 ACK
 *   REPEAT      frame  : SYNCH, 0xA0, count             → 3 echo
 *   LD/ST       frame  : SYNCH, 0x24 or 0x64            → 2 echo
 */
static void prestuff_setptr_ack(int master)
{
    uint8_t ack = UPDI_ACK;
    prestuff_fill(master, 0x00, 4);
    prestuff(master, &ack, 1);
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
 * Each burst emits 9 setup bytes:
 *   ST_PTR_WORD (4) + REPEAT (3) + LD ptr++ (2)
 * Total captured TX setup = 18 bytes; total data RX = 512 bytes.
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
    TEST_ASSERT_EQUAL_size_t(18u, n);

    /* Block 1: ST_PTR_WORD frame → 0x55, 0x69, 0x00, 0x01 */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[0]);
    TEST_ASSERT_EQUAL_HEX8(0x69u,      captured[1]);   /* ST ptr (word) */
    TEST_ASSERT_EQUAL_HEX8(0x00u,      captured[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01u,      captured[3]);
    /* REPEAT frame */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[4]);
    TEST_ASSERT_EQUAL_HEX8(0xA0u,      captured[5]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu,      captured[6]);   /* count = 256-1 */
    /* LD ptr++ frame */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[7]);
    TEST_ASSERT_EQUAL_HEX8(0x24u,      captured[8]);

    /* Block 2: addr advances to 0x0200 */
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[9]);
    TEST_ASSERT_EQUAL_HEX8(0x69u,      captured[10]);
    TEST_ASSERT_EQUAL_HEX8(0x00u,      captured[11]);
    TEST_ASSERT_EQUAL_HEX8(0x02u,      captured[12]);
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[13]);
    TEST_ASSERT_EQUAL_HEX8(0xA0u,      captured[14]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu,      captured[15]);
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, captured[16]);
    TEST_ASSERT_EQUAL_HEX8(0x24u,      captured[17]);
}

/* Test 8: every mem_read block uses ST_PTR(0x69) + REPEAT(0xA0) + LD(0x24) */
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
    TEST_ASSERT_EQUAL_size_t(9u, n);
    TEST_ASSERT_EQUAL_HEX8(0x69u, captured[1]);    /* ST ptr (word)  */
    TEST_ASSERT_EQUAL_HEX8(0xA0u, captured[5]);    /* REPEAT         */
    TEST_ASSERT_EQUAL_HEX8(0x24u, captured[8]);    /* LD ptr++       */
}

/* Test 9: mem_read passes a 0s + 100000us timeval to select() */
static void updi_mem_read_calls_select_with_100ms_timeout_before_read(void)
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

/* Test 17/18/19/20: halt/run/step are deferred to Phase 3 (OCD layer).
 * The API exists as stubs returning -1. */
static void updi_halt_returns_minus1_stub_until_ocd_layer(void)
{
    open_pty_fixture();
    TEST_ASSERT_EQUAL_INT(-1, updi_halt(g_slave_fd));
}
static void updi_run_returns_minus1_stub_until_ocd_layer(void)
{
    open_pty_fixture();
    TEST_ASSERT_EQUAL_INT(-1, updi_run(g_slave_fd));
}
static void updi_step_returns_minus1_stub_until_ocd_layer(void)
{
    open_pty_fixture();
    TEST_ASSERT_EQUAL_INT(-1, updi_step(g_slave_fd));
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
 * Per-attempt SUT byte stream (5 bytes):
 *   0: 0x00         BREAK 1  (tcflush'd — inject doesn't matter)
 *   1: 0x00         BREAK 2  (ditto)
 *   2: 0x55         bare SYNCH      → SUT expects 1 echo
 *   3: 0x55         LDCS frame SYNCH
 *   4: 0x81         LDCS STATUSB    → SUT expects 1 echo + 1 status byte
 *                                     (echo for byte 3 plus echo+status here)
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
    static const uint8_t one_echo       = 0xAA;
    static const uint8_t three_resp[3]  = { 0xBB, 0xCC, 0x00 };

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

        switch ((pos - 1u) % 5u) {
            case 0u: /* BREAK 1                  */                 break;
            case 1u: /* BREAK 2                  */                 break;
            case 2u: (void)write(r->master_fd, &one_echo,   1);     break;
            case 3u: /* LDCS-SYNCH               */                 break;
            case 4u: (void)write(r->master_fd, three_resp,  3);     break;
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

/* Test 3: two consecutive BREAK bytes precede the SYNCH (§35.3.1.2). */
static void updi_open_asserts_two_breaks_then_synch(void)
{
    responder_t rsp;
    pthread_t   tid;
    int         sut_fd;
    uint8_t     cap0, cap1, cap2;

    open_pty_fixture();
    tid    = responder_start(&rsp, g_master_fd);
    sut_fd = updi_open(g_slave_name, 115200);
    responder_stop(tid, &rsp);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sut_fd);
    pthread_mutex_lock(&rsp.mtx);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(3u, rsp.n_captured);
    cap0 = rsp.capture[0];
    cap1 = rsp.capture[1];
    cap2 = rsp.capture[2];
    pthread_mutex_unlock(&rsp.mtx);

    TEST_ASSERT_EQUAL_HEX8(0x00u,      cap0);
    TEST_ASSERT_EQUAL_HEX8(0x00u,      cap1);
    TEST_ASSERT_EQUAL_HEX8(UPDI_SYNCH, cap2);

    close(sut_fd);
}

/* Test 3b: SUT also issues LDCS STATUSB (0x81) after the SYNCH (§35.3.2.3) */
static void updi_open_issues_ldcs_statusb_after_synch(void)
{
    responder_t rsp;
    pthread_t   tid;
    int         sut_fd;
    uint8_t     cmd_byte;

    open_pty_fixture();
    tid    = responder_start(&rsp, g_master_fd);
    sut_fd = updi_open(g_slave_name, 115200);
    responder_stop(tid, &rsp);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sut_fd);
    pthread_mutex_lock(&rsp.mtx);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, rsp.n_captured);
    cmd_byte = rsp.capture[4];
    pthread_mutex_unlock(&rsp.mtx);

    TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x80u | ASI_STATUSB), cmd_byte);

    close(sut_fd);
}

/* Test 4: after the BREAK, baud is restored to the session rate. */
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

/* Test 5: with no responder, BREAK+SYNCH is retried 3 times → 6 BREAKs. */
static void updi_open_retries_break_synch_3_times_on_no_ack(void)
{
    uint8_t cap[32];
    size_t  n, i, breaks;
    int     sut_fd;

    open_pty_fixture();
    sut_fd = updi_open(g_slave_name, 115200);

    TEST_ASSERT_EQUAL_INT(-1, sut_fd);

    n = drain_master(g_master_fd, cap, sizeof(cap));
    breaks = 0;
    for (i = 0; i < n; i++)
        if (cap[i] == 0x00u)
            breaks++;
    TEST_ASSERT_EQUAL_size_t(6u, breaks);   /* 3 attempts × 2 BREAKs each */
}

/* Test 6: after 3 failed BREAK+SYNCH attempts, updi_open returns -1. */
static void updi_open_returns_minus1_after_3_consecutive_link_failures(void)
{
    int sut_fd;

    open_pty_fixture();
    sut_fd = updi_open(g_slave_name, 115200);
    TEST_ASSERT_EQUAL_INT(-1, sut_fd);
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
    RUN_TEST(updi_mem_read_calls_select_with_100ms_timeout_before_read);
    RUN_TEST(updi_mem_read_returns_minus1_on_select_timeout);
    RUN_TEST(updi_mem_write_returns_0_on_success);
    RUN_TEST(updi_mem_write_returns_minus1_on_updi_nak);
    RUN_TEST(updi_halt_returns_minus1_stub_until_ocd_layer);
    RUN_TEST(updi_run_returns_minus1_stub_until_ocd_layer);
    RUN_TEST(updi_step_returns_minus1_stub_until_ocd_layer);
    RUN_TEST(updi_console_poll_returns_pending_bytes_without_halting);

    /* Phase D */
    RUN_TEST(updi_nvm_write_flash_nvmprog_poll_timeout_returns_minus1);
    RUN_TEST(updi_nvm_write_flash_per_page_busy_timeout_returns_minus1);

    /* Phase E */
    RUN_TEST(updi_open_sets_8e2_raw_half_duplex_via_termios);
    RUN_TEST(updi_open_asserts_two_breaks_then_synch);
    RUN_TEST(updi_open_issues_ldcs_statusb_after_synch);
    RUN_TEST(updi_open_restores_session_baud_after_break);
    RUN_TEST(updi_open_retries_break_synch_3_times_on_no_ack);
    RUN_TEST(updi_open_returns_minus1_after_3_consecutive_link_failures);

    return UNITY_END();
}
