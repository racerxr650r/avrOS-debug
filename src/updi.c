/* src/updi.c — UPDI physical-layer implementation (Phase G: spec-aligned) */
#include "updi.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ── UPDI opcode constants (datasheet §35.3.3, Fig. 35-6) ─────────────── */
#define UPDI_OP_ST_PTR_WORD  0x69u   /* ST  pointer-reg, addr size = word    */
#define UPDI_OP_LD_PTR_INC   0x24u   /* LD  rd, *(ptr++)  size B = byte      */
#define UPDI_OP_ST_PTR_INC   0x64u   /* ST  *(ptr++), rr  size B = byte      */
#define UPDI_OP_REPEAT       0xA0u   /* REPEAT count      size B = byte      */
#define UPDI_OP_LDCS_PREFIX  0x80u   /* LDCS cs : 0x80 | cs                  */
#define UPDI_OP_STCS_PREFIX  0xC0u   /* STCS cs : 0xC0 | cs                  */
#define UPDI_OP_KEY          0xE0u   /* KEY (64-bit key payload)             */

/* ── ASI control / status bit positions (datasheet §35.5.9) ────────────── */
#define ASI_SYS_STATUS_NVMPROG 0x08u /* bit 3 — NVM programming active       */
/* NB: §35.5.9 defines no halt/stopped bit in ASI_SYS_STATUS. CPU halt /
 *     step / run live in the OCD register space (separate spec) and are
 *     deferred to Phase 3.                                                 */

/* ── ASI_RESET_REQ values (datasheet §35.5.6) ──────────────────────────── */
#define ASI_RESET_REQ_RUN    0x00u   /* clear reset                          */
#define ASI_RESET_REQ_RESET  0x59u   /* assert system reset                  */

/* ── NVMCTRL register map / bits / sizing ──────────────────────────────── */
#define NVMCTRL_CTRLA        0x1000u
#define NVMCTRL_STATUS       0x1002u
#define NVMCTRL_CMD_ERWP     0x03u   /* erase + write page                   */
#define NVMCTRL_STATUS_BUSY  0x01u   /* bit 0 = BUSY                         */
#define NVMCTRL_STATUS_WRERR 0x04u   /* bit 2 = WRERROR                      */

#define UPDI_FLASH_PAGE_SIZE 512u

/* ── poll counts (each iteration = 1 ms via nanosleep) ─────────────────── */
#define UPDI_NVMPROG_POLL_MAX    100   /* 100 ms */
#define UPDI_PAGE_BUSY_POLL_MAX   20   /*  20 ms */
#define UPDI_READ_TIMEOUT_US  100000   /* 100 ms select() timeout */

/* ── private forward declarations ──────────────────────────────────────── */

static speed_t baud_to_speed(int baud);
static int     updi_write_bytes(int fd, const uint8_t *buf, size_t n);
static int     updi_ldcs(int fd, uint8_t cs);
static int     updi_stcs(int fd, uint8_t cs, uint8_t val);
static int     updi_set_ptr(int fd, uint32_t addr);
static int     updi_send_repeat(int fd, uint8_t count_m1);

/* ── private helpers ────────────────────────────────────────────────────── */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 300:    return B300;
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return B115200;
    }
}

/*
 * Write n bytes to fd, then read and discard exactly n half-duplex echo
 * bytes.  Returns 0 on success, -1 if the echo does not arrive within the
 * VTIME window.  Max n is 16 bytes (largest single UPDI frame).
 */
static int updi_write_bytes(int fd, const uint8_t *buf, size_t n)
{
    uint8_t echo[16];
    size_t  total;

    if (n > sizeof(echo))
        return -1;
    if (write(fd, buf, n) != (ssize_t)n)
        return -1;

    total = 0;
    while (total < n) {
        ssize_t got = read(fd, echo + total, n - total);
        if (got <= 0)
            return -1;
        total += (size_t)got;
    }
    return 0;
}

/* SYNCH + (0x80|cs); reads the one-byte response.  Returns byte or -1. */
static int updi_ldcs(int fd, uint8_t cs)
{
    uint8_t cmd[2] = { UPDI_SYNCH, (uint8_t)(UPDI_OP_LDCS_PREFIX | cs) };
    uint8_t resp;

    if (updi_write_bytes(fd, cmd, 2) < 0)
        return -1;
    if (read(fd, &resp, 1) != (ssize_t)1)
        return -1;
    return (int)(unsigned int)resp;
}

/* SYNCH + (0xC0|cs) + val.  Returns 0 on success, -1 on error. */
static int updi_stcs(int fd, uint8_t cs, uint8_t val)
{
    uint8_t cmd[3] = { UPDI_SYNCH, (uint8_t)(UPDI_OP_STCS_PREFIX | cs), val };
    return updi_write_bytes(fd, cmd, 3);
}

/*
 * Set the UPDI pointer register to a 16-bit address via
 *   SYNCH, ST_PTR_WORD, addr_lo, addr_hi
 * Expects one ACK back from the UPDI (datasheet §35.3.3.4).
 */
static int updi_set_ptr(int fd, uint32_t addr)
{
    uint8_t frame[4] = {
        UPDI_SYNCH,
        UPDI_OP_ST_PTR_WORD,
        (uint8_t)(addr & 0xFFu),
        (uint8_t)((addr >> 8) & 0xFFu)
    };
    uint8_t ack;

    if (updi_write_bytes(fd, frame, sizeof(frame)) < 0)
        return -1;
    if (read(fd, &ack, 1) != (ssize_t)1)
        return -1;
    if (ack != UPDI_ACK)
        return -1;
    return 0;
}

/* SYNCH + REPEAT + count.  REPEAT does not produce an ACK. */
static int updi_send_repeat(int fd, uint8_t count_m1)
{
    uint8_t frame[3] = { UPDI_SYNCH, UPDI_OP_REPEAT, count_m1 };
    return updi_write_bytes(fd, frame, sizeof(frame));
}

/* ── public API ─────────────────────────────────────────────────────────── */

int updi_open(const char *device, int baud)
{
    struct termios tty;
    speed_t        speed;
    int            fd;
    int            attempt;

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (fcntl(fd, F_SETFL, 0) < 0) {
        close(fd);
        return -1;
    }

    if (tcgetattr(fd, &tty) < 0) {
        close(fd);
        return -1;
    }

    /* raw mode (cfmakeraw equivalent) */
    tty.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag &= ~(tcflag_t)OPOST;
    tty.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /* 8E2: 8 data bits, even parity, 2 stop bits.
     *   PARENB = 1, PARODD = 0 ⇒ even parity (datasheet §35.3.1).        */
    tty.c_cflag &= ~(tcflag_t)CSIZE;
    tty.c_cflag &= ~(tcflag_t)PARODD;
    tty.c_cflag |= CS8 | CSTOPB | PARENB | CLOCAL | CREAD;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;   /* 100 ms read timeout */

    speed = baud_to_speed(baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    /* On Linux PTYs the kernel rejects PARENB on a reopened slave
     * (parity is unsupported on virtual terminals).  Try 8E2 first —
     * required by the UPDI spec on real serial ports — and fall back to
     * 8N2 only if the kernel refuses the parity bit.                    */
    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        if (errno != EINVAL) {
            close(fd);
            return -1;
        }
        tty.c_cflag &= ~(tcflag_t)PARENB;
        if (tcsetattr(fd, TCSANOW, &tty) < 0) {
            close(fd);
            return -1;
        }
    }

    /*
     * Recovery handshake (datasheet §35.3.1.2 + §35.3.2.3):
     *   1. Send two consecutive BREAK characters.  The spec recommends
     *      dropping to a very low baud (≤ 300 Bd) so the low period easily
     *      exceeds 12 UPDI-clock bit-times in worst-case low-power modes,
     *      but at session baud (115200, 8E2) a 0x00 frame already holds
     *      the line low for ≈104 µs — well above the 12-bit-time minimum
     *      at the UPDI clock rates (4–32 MHz) used in this debugger.  We
     *      stay at session baud because Linux PTYs flush the master's
     *      RX queue on tcsetattr-baud-change, breaking the unit tests.
     *   2. Send SYNCH (0x55).
     *   3. Read PESIG via LDCS STATUSB to clear any error condition.
     */
    for (attempt = 0; attempt < 3; attempt++) {
        uint8_t brk_pat[2] = { 0x00u, 0x00u };
        uint8_t synch      = UPDI_SYNCH;

        if (write(fd, brk_pat, sizeof(brk_pat)) != (ssize_t)sizeof(brk_pat)) {
            continue;
        }
        tcdrain(fd);
        /* Discard the half-duplex echo of the BREAK pattern (and any
         * spurious bytes the kernel queued while the line settled).
         * Without this flush the leftover BREAK echo bytes shift the
         * echo-byte accounting in every subsequent updi_write_bytes()
         * call by one byte, which causes the first updi_mem_read() to
         * mistake an echo byte for the ACK and abort.  Real UARTs
         * (e.g. Raspberry Pi PL011) reproduce this in any session that
         * runs back-to-back with the SIGROW read.                     */
        tcflush(fd, TCIFLUSH);

        if (updi_write_bytes(fd, &synch, 1) < 0)
            continue;
        if (updi_ldcs(fd, ASI_STATUSB) >= 0)
            return fd;
    }

    close(fd);
    return -1;
}

void updi_close(int fd)
{
    if (fd < 0)
        return;
    tcdrain(fd);
    close(fd);
}

/*
 * mem_read: split into three frames per block (datasheet §35.3.3.3,
 * §35.3.3.4, §35.3.3.7):
 *     A) SYNCH, ST_PTR_WORD, addr_lo, addr_hi  → ACK
 *     B) SYNCH, REPEAT, count-1                (no ACK)
 *     C) SYNCH, LD ptr++                       (no ACK)
 * The UPDI then streams `count` data bytes after the guard time.
 */
int updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        size_t         block_len = len - done;
        uint32_t       cur_addr  = addr + (uint32_t)done;
        fd_set         rfds;
        struct timeval tv;
        size_t         got;
        uint8_t        ld_frame[2] = { UPDI_SYNCH, UPDI_OP_LD_PTR_INC };

        if (block_len > UPDI_MAX_BLOCK)
            block_len = UPDI_MAX_BLOCK;

        if (updi_set_ptr(fd, cur_addr) < 0)
            return -1;
        if (updi_send_repeat(fd, (uint8_t)(block_len - 1u)) < 0)
            return -1;
        if (updi_write_bytes(fd, ld_frame, sizeof(ld_frame)) < 0)
            return -1;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = UPDI_READ_TIMEOUT_US;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            return -1;

        got = 0;
        while (got < block_len) {
            ssize_t r = read(fd, buf + done + got, block_len - got);
            if (r <= 0)
                return -1;
            got += (size_t)r;
        }
        done += block_len;
    }
    return 0;
}

/*
 * mem_write: same three-frame setup as mem_read, then per-byte data with
 * one ACK from the UPDI per byte (datasheet §35.3.3.4 + Fig. 35-13).
 */
int updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        size_t   block_len = len - done;
        uint32_t cur_addr  = addr + (uint32_t)done;
        size_t   i;
        uint8_t  st_frame[2] = { UPDI_SYNCH, UPDI_OP_ST_PTR_INC };

        if (block_len > UPDI_MAX_BLOCK)
            block_len = UPDI_MAX_BLOCK;

        if (updi_set_ptr(fd, cur_addr) < 0)
            return -1;
        if (updi_send_repeat(fd, (uint8_t)(block_len - 1u)) < 0)
            return -1;
        if (updi_write_bytes(fd, st_frame, sizeof(st_frame)) < 0)
            return -1;

        for (i = 0; i < block_len; i++) {
            uint8_t ack;
            if (updi_write_bytes(fd, &buf[done + i], 1) < 0)
                return -1;
            if (read(fd, &ack, 1) != (ssize_t)1)
                return -1;
            if (ack != UPDI_ACK)
                return -1;
        }
        done += block_len;
    }
    return 0;
}

/* ── CPU halt / single-step / run: deferred to Phase 3 (OCD layer) ────── *
 *                                                                         *
 * Datasheet §35.5 does not define halt/step/run bits in ASI register      *
 * space.  These operations live in the OCD register space which is        *
 * documented separately.  The API is retained so higher layers can link,  *
 * but every call returns -1 until the OCD layer is implemented.           */

int updi_halt(int fd) { (void)fd; return -1; }
int updi_run(int fd)  { (void)fd; return -1; }
int updi_step(int fd) { (void)fd; return -1; }

int updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data,
                         size_t len)
{
    static const uint8_t key_cmd[10] = {
        UPDI_SYNCH, UPDI_OP_KEY,
        'N', 'V', 'M', 'P', 'r', 'o', 'g', ' '    /* 8-byte NVM key */
    };
    struct timespec ts = { 0, 1000000L };
    size_t          n_pages;
    size_t          pg;
    int             ready;

    if ((word_addr % UPDI_FLASH_PAGE_SIZE) != 0u ||
        len == 0u || (len % UPDI_FLASH_PAGE_SIZE) != 0u)
        return -1;

    /* 1. Send NVMPROG KEY (datasheet §35.3.7.2 step 2) */
    if (updi_write_bytes(fd, key_cmd, sizeof(key_cmd)) < 0)
        return -1;

    /* 2. Assert system reset, then clear it (steps 4-5) */
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;

    /* 3. Poll ASI_SYS_STATUS.NVMPROG (bit 3) until set (steps 6-7)        */
    ready = 0;
    for (int i = 0; i < UPDI_NVMPROG_POLL_MAX; i++) {
        int s = updi_ldcs(fd, ASI_SYS_STATUS);
        if (s < 0)
            return -1;
        if (s & ASI_SYS_STATUS_NVMPROG) {
            ready = 1;
            break;
        }
        nanosleep(&ts, NULL);
    }
    if (!ready)
        return -1;

    /* 4. Erase + program each page (step 8) */
    n_pages = len / UPDI_FLASH_PAGE_SIZE;
    for (pg = 0; pg < n_pages; pg++) {
        uint32_t       page_addr = word_addr +
                                   (uint32_t)(pg * UPDI_FLASH_PAGE_SIZE);
        const uint8_t *page_buf  = data + pg * UPDI_FLASH_PAGE_SIZE;
        uint8_t        nvm_cmd   = NVMCTRL_CMD_ERWP;
        int            busy_done = 0;

        if (updi_mem_write(fd, page_addr, page_buf,
                           UPDI_FLASH_PAGE_SIZE) < 0)
            return -1;

        if (updi_mem_write(fd, NVMCTRL_CTRLA, &nvm_cmd, 1u) < 0)
            return -1;

        for (int i = 0; i < UPDI_PAGE_BUSY_POLL_MAX; i++) {
            uint8_t nvm_st = 0;
            if (updi_mem_read(fd, NVMCTRL_STATUS, &nvm_st, 1u) < 0)
                return -1;
            if (nvm_st & NVMCTRL_STATUS_WRERR)
                return UPDI_ERR_WP;
            if (!(nvm_st & NVMCTRL_STATUS_BUSY)) {
                busy_done = 1;
                break;
            }
            nanosleep(&ts, NULL);
        }
        if (!busy_done)
            return -1;
    }

    /* 5. Exit programming: reset + clear (datasheet §35.3.7.2 steps 9-10) */
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;

    return 0;
}

int updi_console_poll(int fd, char *buf, size_t cap)
{
    fd_set         rfds;
    struct timeval tv = { 0, 0 };   /* zero timeout — non-blocking */
    int            sel;
    ssize_t        n;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    sel = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sel < 0)
        return -1;
    if (sel == 0)
        return 0;

    n = read(fd, buf, cap);
    if (n < 0)
        return -1;
    return (int)n;
}
