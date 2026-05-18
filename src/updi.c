/* src/updi.c — UPDI physical-layer implementation (Phase G: spec-aligned) */
#include "updi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ── UPDI opcode constants (datasheet §35.3.3, Fig. 35-6) ─────────────── */
#define UPDI_OP_LDS_24_8     0x08u   /* LDS  addr=24b, data= 8b               */
#define UPDI_OP_STS_24_8     0x48u   /* STS  addr=24b, data= 8b               */
#define UPDI_OP_ST_PTR_WORD  0x69u   /* ST  pointer-reg, addr size = word (16b) */
#define UPDI_OP_ST_PTR_LONG  0x6Au   /* ST  pointer-reg, addr size = long (24b) */
#define UPDI_OP_LD_PTR_INC   0x24u   /* LD  rd, *(ptr++)  size B = byte      */
#define UPDI_OP_ST_PTR_INC   0x64u   /* ST  *(ptr++), rr  size B = byte      */
#define UPDI_OP_ST_PTR_INC_W 0x65u   /* ST  *(ptr++), rr  size W = word      */
#define UPDI_OP_REPEAT       0xA0u   /* REPEAT count      size B = byte      */
#define UPDI_OP_LDCS_PREFIX  0x80u   /* LDCS cs : 0x80 | cs                  */
#define UPDI_OP_STCS_PREFIX  0xC0u   /* STCS cs : 0xC0 | cs                  */
#define UPDI_OP_KEY          0xE0u   /* KEY (64-bit key payload)             */

/* CTRLA values used during bulk page-buffer programming (matches avrdude).  *
 *   RSD_ON:  bit3 RSD=1 (Response Signature Disable) + GTVAL=6              *
 *   RSD_OFF: bit3 RSD=0                              + GTVAL=6              */
#define UPDI_CTRLA_RSD_ON    0x0Eu
#define UPDI_CTRLA_RSD_OFF   0x06u

/* ── ASI control / status bit positions (datasheet §35.5.9) ────────────── */
#define ASI_SYS_STATUS_NVMPROG 0x08u /* bit 3 — NVM programming active       */
/* NB: §35.5.9 defines no halt/stopped bit in ASI_SYS_STATUS. CPU halt /
 *     step / run live in the OCD register space (separate spec) and are
 *     deferred to Phase 3.                                                 */

/* ── ASI_RESET_REQ values (datasheet §35.5.6) ──────────────────────────── */
#define ASI_RESET_REQ_RUN    0x00u   /* clear reset                          */
#define ASI_RESET_REQ_RESET  0x59u   /* assert system reset                  */

/* ── NVMCTRL register map / bits / sizing (AVR-Dx, per Atmel.AVR-Dx_DFP) ── */
#define NVMCTRL_CTRLA        0x1000u
#define NVMCTRL_STATUS       0x1002u
#define NVMCTRL_CMD_NOCMD    0x00u   /* clear pending command                */
#define NVMCTRL_CMD_FLWR     0x02u   /* program page buffer to FLASH         */
#define NVMCTRL_CMD_FLPER    0x08u   /* erase one FLASH page                 */
#define NVMCTRL_CMD_CHER     0x20u   /* chip erase (full FLASH)              */
#define NVMCTRL_STATUS_FBUSY 0x01u   /* bit 0 = FLASH busy                   */
#define NVMCTRL_STATUS_EEBUSY 0x02u  /* bit 1 = EEPROM busy                  */

/* UPDI_FLASH_PAGE_SIZE is declared in updi.h */

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
 * Set the UPDI pointer register via
 *   16-bit form: SYNCH, ST_PTR_WORD (0x69), addr_lo, addr_hi
 *   24-bit form: SYNCH, ST_PTR_LONG (0x6A), addr_lo, addr_mid, addr_hi
 *
 * The 24-bit form is required for the AVR-Dx mapped-Flash region above
 * 0x0000FFFF on parts with > 64 KiB Flash (AVR128DA/DB) and is selected
 * automatically when `addr > 0xFFFF`. Sub-64-KiB targets (SIGROW, NVMCTRL,
 * SRAM, ASI registers, etc.) keep the 16-bit form for byte-for-byte
 * compatibility with avrdude's serialupdi cold-path captures.
 *
 * Both forms expect one ACK back from the UPDI (datasheet §35.3.3.4).
 */
static int updi_set_ptr(int fd, uint32_t addr)
{
    uint8_t frame[5];
    size_t  frame_len;
    uint8_t ack;

    frame[0] = UPDI_SYNCH;
    if (addr > 0xFFFFu) {
        frame[1] = UPDI_OP_ST_PTR_LONG;
        frame[2] = (uint8_t)( addr        & 0xFFu);
        frame[3] = (uint8_t)((addr >>  8) & 0xFFu);
        frame[4] = (uint8_t)((addr >> 16) & 0xFFu);
        frame_len = 5u;
    } else {
        frame[1] = UPDI_OP_ST_PTR_WORD;
        frame[2] = (uint8_t)( addr       & 0xFFu);
        frame[3] = (uint8_t)((addr >> 8) & 0xFFu);
        frame_len = 4u;
    }

    if (updi_write_bytes(fd, frame, frame_len) < 0)
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

/*
 * Enter NVMPROG mode (datasheet §35.3.7.2 steps 2-7).
 *
 * Sends the 8-byte "NVMProg " KEY (LSB-first), pulses ASI_RESET_REQ to
 * latch the key, then polls ASI_SYS_STATUS.NVMPROG (bit 3) until set.
 *
 * Holding the target in NVMPROG keeps the CPU halted, which is required
 * any time the host needs reliable repeated memory accesses — otherwise
 * a target running firmware that enters SLEEP (e.g. `_exit` → `cli; sleep`
 * on a bare-metal fixture) will fail every UPDI read after the initial
 * cold-start handshake.  Both avrdude's serialupdi and pymcuprog enter
 * NVMPROG before any user-visible memory access for exactly this reason.
 *
 * Returns 0 on success, -1 on I/O error or timeout.
 */
static int updi_enter_nvmprog(int fd)
{
    static const uint8_t key_cmd[10] = {
        UPDI_SYNCH, UPDI_OP_KEY,
        /* "NVMProg ", LSB-first per datasheet §35.3.3.13 */
        ' ', 'g', 'o', 'r', 'P', 'M', 'V', 'N'
    };
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    int last_status = -1;

    if (updi_write_bytes(fd, key_cmd, sizeof(key_cmd)) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;

    for (int i = 0; i < UPDI_NVMPROG_POLL_MAX; i++) {
        int s = updi_ldcs(fd, ASI_SYS_STATUS);
        if (s < 0)
            return -1;
        last_status = s;
        if (s & ASI_SYS_STATUS_NVMPROG)
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr,
            "updi_enter_nvmprog: NVMPROG bit never set (last SYS_STATUS=0x%02x)\n",
            last_status & 0xFF);
    return -1;
}

/*
 * LDS (24-bit address, 8-bit data): direct single-byte load from any
 * 24-bit physical address.  Wire: SYNCH, 0x08, addr_lo, addr_mid, addr_hi.
 * Target responds with one data byte.  Used for NVMCTRL / SIGROW access
 * where avrdude's serialupdi uses LDS rather than ST_PTR+LD_PTR_INC.
 */
static int updi_lds8(int fd, uint32_t addr, uint8_t *out)
{
    uint8_t frame[5] = {
        UPDI_SYNCH, UPDI_OP_LDS_24_8,
        (uint8_t)( addr        & 0xFFu),
        (uint8_t)((addr >>  8) & 0xFFu),
        (uint8_t)((addr >> 16) & 0xFFu),
    };
    if (updi_write_bytes(fd, frame, sizeof(frame)) < 0)
        return -1;
    if (read(fd, out, 1) != (ssize_t)1)
        return -1;
    return 0;
}

/*
 * STS (24-bit address, 8-bit data): direct single-byte store to any
 * 24-bit physical address.  Wire: SYNCH, 0x48, addr_lo, addr_mid, addr_hi;
 * target ACKs; then send data byte; target ACKs.  Used for NVMCTRL writes.
 */
static int updi_sts8(int fd, uint32_t addr, uint8_t val)
{
    uint8_t frame[5] = {
        UPDI_SYNCH, UPDI_OP_STS_24_8,
        (uint8_t)( addr        & 0xFFu),
        (uint8_t)((addr >>  8) & 0xFFu),
        (uint8_t)((addr >> 16) & 0xFFu),
    };
    uint8_t ack;

    if (updi_write_bytes(fd, frame, sizeof(frame)) < 0)
        return -1;
    if (read(fd, &ack, 1) != (ssize_t)1 || ack != UPDI_ACK)
        return -1;
    if (updi_write_bytes(fd, &val, 1) < 0)
        return -1;
    if (read(fd, &ack, 1) != (ssize_t)1 || ack != UPDI_ACK)
        return -1;
    return 0;
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
     * Cold-start handshake (datasheet §35.3.1.2 + §35.3.2.3, matching
     * avrdude's `serialupdi` programmer; see issue #19):
     *
     *   1. Write one 0x00 wake byte.  Its low frame doubles as a soft
     *      BREAK at session baud — ≈87 µs of line-low, well above the
     *      UPDI 12-bit-time minimum at any UPDI-clock rate in use.
     *   2. tcdrain to flush TX, then tcflush(TCIFLUSH) to drop the
     *      half-duplex echo of the wake byte.  Without the flush the
     *      stale echo shifts the echo-byte accounting in every later
     *      `updi_write_bytes()` call by one byte, which makes the
     *      first `updi_set_ptr()` read an echo byte where it expects
     *      the ACK and abort.
     *   3. `STCS ASI_CTRLB = CCDETDIS` so the target ignores the
     *      contention between its open-drain UPDI TX and the host
     *      UART's push-pull idle-high TX.  Passive single-wire
     *      combiners (the typical wiring on Raspberry Pi PL011 / FTDI
     *      hosts) make the target see every TX attempt as a collision
     *      and stay silent forever unless CCDETDIS is set.
     *   4. `STCS ASI_CTRLA = IBDLY` so the target inserts inter-byte
     *      guard time on its responses, which the host UART RX needs
     *      to deframe reliably at session baud.
     *   5. `LDCS ASI_STATUSA` to verify the link is alive.  STATUSA
     *      carries UPDIREV in its upper nibble and is always readable
     *      once UPDI is enabled, even before any reset has occurred.
     *
     * Retries: the entire 5-step sequence is attempted up to 3 times
     * before declaring a link failure and returning -1.
     */
    /*
     * Cold-start handshake (matching avrdude's `serialupdi` programmer
     * exactly; see issue #19).  Empirically verified by `strace`-ing
     * avrdude against the same target as this debugger:
     *
     *   Attempt 0 (fast path, succeeds against an already-prepped target):
     *     1. Write one 0x00 wake byte at session baud, then
     *        tcdrain() + tcflush(TCIFLUSH) to discard the half-duplex
     *        echo of the wake byte.
     *     2. STCS ASI_CTRLB = CCDETDIS  (disable contention detection)
     *     3. STCS ASI_CTRLA = IBDLY     (inter-byte delay on responses)
     *     4. LDCS ASI_STATUSA          (link probe)
     *
     *   Attempts 1, 2 (slow path, required on cold power-on with passive
     *   single-wire combiners — RPi PL011, FTDI):
     *     1a. Switch the kernel baud rate to 300 baud.
     *     1b. Write two 0x00 bytes — each holds the TX line low for
     *         ≈ 33 ms at 300 baud, giving the target's UPDI clock and
     *         contention detector a clean ≥ 60 ms reset window that
     *         dwarfs anything achievable at session baud.
     *     1c. Restore session baud and tcflush(TCIFLUSH).
     *     2-4. STCS CTRLB / STCS CTRLA / LDCS STATUSA as above.
     *
     * The 300-baud trick is unusual but Microchip-blessed — pymcuprog
     * and avrdude both use it because no portable POSIX API generates
     * a multi-ms BREAK on demand.  tcsendbreak() is too short and
     * not honoured by all kernel UART drivers.
     */
    for (attempt = 0; attempt < 3; attempt++) {
        int rc;

        if (attempt == 0) {
            uint8_t wake = 0x00u;

            if (write(fd, &wake, 1) != (ssize_t)1)
                continue;
            tcdrain(fd);
            tcflush(fd, TCIFLUSH);
        } else {
            struct termios slow = tty;
            uint8_t        brk  = 0x00u;
            uint8_t        echo;

            /* B300, 8 data bits, even parity, 1 stop bit (NOT 2) — exactly
             * matching avrdude's serialupdi.  The 1-stop-bit cell makes the
             * byte ≈ 33 ms long, which is the line-low BREAK duration we
             * want.  CSTOPB is restored when we revert to session baud. */
            cfsetispeed(&slow, B300);
            cfsetospeed(&slow, B300);
            slow.c_cflag &= ~(tcflag_t)CSTOPB;
            if (tcsetattr(fd, TCSADRAIN, &slow) < 0)
                continue;
            tcflush(fd, TCIFLUSH);

            /* Two 0x00 bytes at 300 baud = two ~33 ms line-low pulses.
             * `tcdrain()` after each write is critical: it blocks until the
             * UART has actually clocked the byte out of the FIFO.  Without
             * it, the subsequent `tcsetattr(B115200)` either flushes the
             * queued byte or re-clocks it at 115200 baud, in either case
             * destroying the BREAK pulse.  After each write+drain we
             * read-and-discard the half-duplex echo at 300 baud; if it
             * fails to arrive within VTIME we still proceed (the BREAK
             * pulse itself is what the target needs, not the echo).      */
            if (write(fd, &brk, 1) == (ssize_t)1) {
                tcdrain(fd);
                { ssize_t r = read(fd, &echo, 1); (void)r; }
            }
            if (write(fd, &brk, 1) == (ssize_t)1) {
                tcdrain(fd);
                { ssize_t r = read(fd, &echo, 1); (void)r; }
            }

            /* Allow any in-flight echo bytes from the line bus to reach
             * the kernel RX queue before we switch baud — otherwise they
             * arrive AFTER the tcflush below, get clocked at 115200 baud
             * (corrupt framing), and desynchronise every subsequent
             * frame echo.                                                */
            {
                struct timespec ts = { 0, 50 * 1000 * 1000L };  /* 50 ms */
                (void)nanosleep(&ts, NULL);
            }
            tcflush(fd, TCIFLUSH);

            if (tcsetattr(fd, TCSADRAIN, &tty) < 0)
                continue;
            tcflush(fd, TCIFLUSH);
        }

        rc = updi_stcs(fd, ASI_CTRLB, ASI_CTRLB_CCDETDIS);
        if (rc < 0) continue;
        rc = updi_stcs(fd, ASI_CTRLA, ASI_CTRLA_IBDLY);
        if (rc < 0) continue;
        rc = updi_ldcs(fd, ASI_STATUSA);
        if (rc < 0) continue;

        /*
         * Wake the target from UPDI SLEEP if necessary.  Reading the
         * 16-byte System Information Block (SYNCH + 0xE6, then 16
         * response bytes) is harmless on an awake target and reliably
         * wakes a sleeping one — avrdude's serialupdi issues exactly
         * this transaction whenever ASI_SYS_STATUS reports INSLEEP.
         * Without this step a target left in SLEEP from a prior session
         * will accept the link-layer probes (LDCS STATUSA) but reject
         * every memory access (sigrow read returns -1).
         */
        {
            uint8_t sib_cmd[2] = { UPDI_SYNCH, UPDI_OP_KEY_SIB };
            uint8_t sib_buf[UPDI_SIB_LEN];
            size_t  total;
            int     ok = 1;

            if (updi_write_bytes(fd, sib_cmd, sizeof(sib_cmd)) < 0)
                continue;
            total = 0;
            while (total < sizeof(sib_buf)) {
                ssize_t got = read(fd, sib_buf + total, sizeof(sib_buf) - total);
                if (got <= 0) { ok = 0; break; }
                total += (size_t)got;
            }
            if (!ok) continue;
        }

        /*
         * Enter NVMPROG to halt the CPU.  This prevents target firmware
         * that immediately enters SLEEP after `main()` returns (or any
         * other power-management code path) from making subsequent UPDI
         * memory reads time out.  We do this unconditionally because
         * even an erased target benefits (its execution of 0xFFFF =
         * undefined opcode is unpredictable) and the chip-erase /
         * NVM-write paths re-issue the NVMPROG key anyway, which is
         * idempotent.
         */
        if (updi_enter_nvmprog(fd) < 0)
            continue;

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
 *     A) SYNCH, ST_PTR_{WORD,LONG}, addr bytes (LE) → ACK
 *     B) SYNCH, REPEAT, count-1                     (no ACK)
 *     C) SYNCH, LD ptr++                            (no ACK)
 * The UPDI then streams `count` data bytes after the guard time.
 * `updi_set_ptr()` selects ST_PTR_WORD (16-bit) for addr ≤ 0xFFFF and
 * ST_PTR_LONG (24-bit) above, enabling access to mapped Flash on
 * AVR128DA/DB parts.
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

/* ── CPU halt / single-step / run ─────────────────────────────────────── *
 *                                                                         *
 * AVR-Dx UPDI exposes no halt/step bits in the ASI register space; full   *
 * CPU control lives in the OCD register space (separate, NDA-only spec)  *
 * and is not implemented here.  We approximate with what UPDI provides:   *
 *                                                                         *
 *   updi_halt() — re-enter NVMPROG mode.  This asserts system reset with  *
 *                 the NVMProg key latched, which leaves the CPU stopped   *
 *                 and gives us full bus access for memory I/O.            *
 *                                                                         *
 *   updi_run()  — pulse system reset without the NVMProg key.  NVMPROG    *
 *                 clears and the CPU starts executing from the reset      *
 *                 vector (0x0000).  Note: this is "reset & run", not a    *
 *                 true "continue from current PC".                        *
 *                                                                         *
 *   updi_step() — true single-step requires OCD; not implemented.         */

int updi_halt(int fd)
{
    return updi_enter_nvmprog(fd);
}

int updi_run(int fd)
{
    /* Reset pulse without re-latching the NVMProg key: NVMPROG clears
     * and the CPU runs from the reset vector.  Until updi_halt() is
     * called, NVMCTRL/SIGROW/SRAM reads via UPDI are not reliable      *
     * because the CPU may concurrently access those buses.            */
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;
    return 0;
}

int updi_step(int fd) { (void)fd; return -1; }

/* Poll NVMCTRL.STATUS until FBUSY clears or the budget is exhausted.
 * AVR-Dx NVMSTATUS exposes only FBUSY (bit 0) and EEBUSY (bit 1); there
 * is no write-error flag, so completion is detected purely by !FBUSY.
 * Returns 0 on success, -1 on timeout / I/O error.                       */
static int nvm_wait_not_busy(int fd, uint32_t page_addr, const char *phase)
{
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    uint8_t last = 0xAAu;
    for (int i = 0; i < 200; i++) {         /* up to 200 ms */
        uint8_t nvm_st = 0;
        if (updi_lds8(fd, NVMCTRL_STATUS, &nvm_st) < 0) {
            fprintf(stderr,
                    "nvm_write_flash: %s NVMSTATUS read failed @0x%06x\n",
                    phase, (unsigned)page_addr);
            return -1;
        }
        last = nvm_st;
        if (!(nvm_st & NVMCTRL_STATUS_FBUSY))
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr,
            "nvm_write_flash: %s FBUSY did not clear @0x%06x (last NVMSTATUS=0x%02x)\n",
            phase, (unsigned)page_addr, last & 0xFF);
    return -1;
}

/*
 * Program one 512-byte FLASH page using the avrdude/pymcuprog sequence:
 *
 *   1. arm NVMCTRL.CTRLA = FLWR via direct STS
 *   2. ST_PTR_LONG ← page_addr (24-bit)
 *   3. STCS ASI_CTRLA = RSD_ON  (target stops ACKing individual stores)
 *   4. REPEAT 0xFF             (256 word-stores follow)
 *   5. ST *(ptr++), word        opcode (0x65) + 512 data bytes
 *   6. STCS ASI_CTRLA = RSD_OFF (restore normal ACK protocol)
 *
 * Each word-store latches into the FLASH page buffer; the AVR-Dx NVM
 * controller commits the buffer to FLASH automatically because FLWR was
 * pre-armed in step 1.  Caller is responsible for waiting FBUSY=0 after.
 */
static int updi_write_page_bulk(int fd, uint32_t page_addr,
                                const uint8_t *data)
{
    uint8_t st_word_frame[2] = { UPDI_SYNCH, UPDI_OP_ST_PTR_INC_W };
    uint8_t echo[64];
    size_t  total;

    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_FLWR) < 0) {
        fprintf(stderr, "updi_write_page_bulk: arm FLWR failed\n");
        return -1;
    }
    if (updi_set_ptr(fd, page_addr) < 0) {
        fprintf(stderr, "updi_write_page_bulk: set_ptr 0x%06x failed\n",
                (unsigned)page_addr);
        return -1;
    }
    if (updi_stcs(fd, ASI_CTRLA, UPDI_CTRLA_RSD_ON) < 0) {
        fprintf(stderr, "updi_write_page_bulk: STCS RSD_ON failed\n");
        return -1;
    }
    if (updi_send_repeat(fd, 0xFFu) < 0) {
        fprintf(stderr, "updi_write_page_bulk: REPEAT failed\n");
        goto restore;
    }
    if (updi_write_bytes(fd, st_word_frame, sizeof(st_word_frame)) < 0) {
        fprintf(stderr, "updi_write_page_bulk: ST_PTR_INC_W failed\n");
        goto restore;
    }

    /* Stream 512 data bytes; no ACK per byte (RSD enabled). */
    if (write(fd, data, UPDI_FLASH_PAGE_SIZE) !=
        (ssize_t)UPDI_FLASH_PAGE_SIZE) {
        fprintf(stderr, "updi_write_page_bulk: page write short\n");
        goto restore;
    }
    total = 0;
    while (total < UPDI_FLASH_PAGE_SIZE) {
        size_t want = UPDI_FLASH_PAGE_SIZE - total;
        if (want > sizeof(echo)) want = sizeof(echo);
        ssize_t got = read(fd, echo, want);
        if (got <= 0) {
            fprintf(stderr,
                    "updi_write_page_bulk: echo read failed after %zu B\n",
                    total);
            goto restore;
        }
        total += (size_t)got;
    }

    if (updi_stcs(fd, ASI_CTRLA, UPDI_CTRLA_RSD_OFF) < 0) {
        fprintf(stderr, "updi_write_page_bulk: STCS RSD_OFF failed\n");
        return -1;
    }
    return 0;

restore:
    /* Best-effort restore — return -1 regardless. */
    (void)updi_stcs(fd, ASI_CTRLA, UPDI_CTRLA_RSD_OFF);
    return -1;
}

int updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data,
                         size_t len)
{
    size_t n_pages;
    size_t pg;

    if ((word_addr % UPDI_FLASH_PAGE_SIZE) != 0u ||
        len == 0u || (len % UPDI_FLASH_PAGE_SIZE) != 0u) {
        fprintf(stderr, "nvm_write_flash: bad align (addr=0x%06x len=%zu)\n",
                (unsigned)word_addr, len);
        return -1;
    }

    /* 1-3. Re-enter NVMPROG (idempotent: updi_open already did it).
     *      Re-issuing the key + reset pulse ensures we are in a clean
     *      programming state even if the caller did `--erase` first,
     *      which leaves the chip in CHIPERASE state.                  */
    if (updi_enter_nvmprog(fd) < 0) {
        fprintf(stderr, "nvm_write_flash: failed to enter NVMPROG\n");
        return -1;
    }

    /* 4. Program each page (matches avrdude/pymcuprog AVR-Dx sequence).
     *    Caller is responsible for chip-erase (--erase) prior to load;
     *    we do not issue per-page FLPER here because FLWR alone commits
     *    pre-erased FLASH page buffers, and avrdude does the same.    */
    n_pages = len / UPDI_FLASH_PAGE_SIZE;
    for (pg = 0; pg < n_pages; pg++) {
        uint32_t       page_addr = word_addr +
                                   (uint32_t)(pg * UPDI_FLASH_PAGE_SIZE);
        const uint8_t *page_buf  = data + pg * UPDI_FLASH_PAGE_SIZE;

        if (nvm_wait_not_busy(fd, page_addr, "pre-write") < 0)
            return -1;
        if (updi_write_page_bulk(fd, page_addr, page_buf) < 0) {
            fprintf(stderr,
                    "nvm_write_flash: page 0x%06x write failed\n",
                    (unsigned)page_addr);
            return -1;
        }
        if (nvm_wait_not_busy(fd, page_addr, "post-write") < 0)
            return -1;
    }

    /* 5. Stay in NVMPROG — the GDB server (or --device) needs the CPU
     *    held halted for subsequent memory reads.  The caller invokes
     *    `updi_close()` to release the chip on shutdown.              */
    return 0;
}

/*
 * Erase a single 512-byte FLASH page (AVR-Dx NVMCTRL FLPER command).
 * AVR-Dx datasheet §6 — FLPER is armed by writing 0x08 to NVMCTRL.CTRLA,
 * then erase is triggered by any FLASH write within the target page.
 * We use a single-byte STS of 0xFF as the trigger.
 * `page_addr` must be the UPDI 24-bit FLASH-space address of the page
 * (i.e. already includes UPDI_FLASH_BASE) and must be page-aligned.
 */
static int nvm_erase_page(int fd, uint32_t page_addr)
{
    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_FLPER) < 0) {
        fprintf(stderr, "nvm_erase_page: arm FLPER failed @0x%06x\n",
                (unsigned)page_addr);
        return -1;
    }
    /* Trigger the erase by writing one byte into the target page. */
    if (updi_sts8(fd, page_addr, 0xFFu) < 0) {
        fprintf(stderr, "nvm_erase_page: trigger STS failed @0x%06x\n",
                (unsigned)page_addr);
        return -1;
    }
    if (nvm_wait_not_busy(fd, page_addr, "erase") < 0)
        return -1;
    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_NOCMD) < 0) {
        fprintf(stderr, "nvm_erase_page: clear CMD failed @0x%06x\n",
                (unsigned)page_addr);
        return -1;
    }
    return 0;
}

/*
 * updi_nvm_flash_patch: read-modify-write FLASH at byte granularity.
 *
 * For each FLASH page touched by [addr, addr+len), the current page is
 * read into a 512-byte buffer, the affected bytes are overwritten, the
 * page is erased (FLPER), and the patched buffer is written back via
 * the bulk page programmer.  Used by the GDB RSP Z0/z0 handlers to
 * plant and remove the AVR BREAK opcode (0x9598) for software
 * breakpoints, where the write is typically 2 bytes at an arbitrary
 * instruction address.
 *
 * `addr` must be a UPDI FLASH-space byte address (i.e. include
 * UPDI_FLASH_BASE).  Length may be any non-zero value; the function
 * spans page boundaries as needed.  The chip remains in NVMPROG on
 * return.
 */
int updi_nvm_flash_patch(int fd, uint32_t addr, const uint8_t *data,
                         size_t len)
{
    const uint32_t page_mask = UPDI_FLASH_PAGE_SIZE - 1u;
    uint8_t  pgbuf[UPDI_FLASH_PAGE_SIZE];
    uint32_t end;
    uint32_t pa;

    if (len == 0u) return 0;

    if (updi_enter_nvmprog(fd) < 0) {
        fprintf(stderr, "nvm_flash_patch: failed to enter NVMPROG\n");
        return -1;
    }

    end = addr + (uint32_t)len;
    for (pa = addr & ~page_mask; pa < end; pa += UPDI_FLASH_PAGE_SIZE) {
        uint32_t pg_end = pa + UPDI_FLASH_PAGE_SIZE;
        uint32_t lo     = (addr > pa)     ? addr : pa;
        uint32_t hi     = (end  < pg_end) ? end  : pg_end;
        size_t   i;

        if (updi_mem_read(fd, pa, pgbuf, UPDI_FLASH_PAGE_SIZE) < 0) {
            fprintf(stderr,
                    "nvm_flash_patch: read page 0x%06x failed\n",
                    (unsigned)pa);
            return -1;
        }
        for (i = 0; i < (hi - lo); i++) {
            pgbuf[(lo - pa) + i] = data[(lo - addr) + i];
        }

        if (nvm_wait_not_busy(fd, pa, "patch-pre") < 0) return -1;
        if (nvm_erase_page(fd, pa) < 0) return -1;
        if (updi_write_page_bulk(fd, pa, pgbuf) < 0) {
            fprintf(stderr,
                    "nvm_flash_patch: write page 0x%06x failed\n",
                    (unsigned)pa);
            return -1;
        }
        if (nvm_wait_not_busy(fd, pa, "patch-post") < 0) return -1;
    }
    return 0;
}

/*
 * updi_chip_erase: send the CHIPERASE KEY ("NVMErase"), assert system
 * reset, then poll ASI_SYS_STATUS until LOCKSTATUS (bit 1) clears.
 *
 * AVR-Dx datasheet §35.3.7.1 — required to unlock a locked device before
 * NVMPROG can be entered.  DESTRUCTIVE: erases all FLASH and clears the
 * lock fuse.
 */
int updi_chip_erase(int fd)
{
    static const uint8_t key_cmd[10] = {
        UPDI_SYNCH, UPDI_OP_KEY,
        /* 8-byte CHIPERASE key "NVMErase", transmitted LSB-first per
         * datasheet §35.3.3.13 (UPDI KEY opcode). */
        'e', 's', 'a', 'r', 'E', 'M', 'V', 'N'
    };
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    int ks;

    /* 1. Send CHIPERASE key */
    if (updi_write_bytes(fd, key_cmd, sizeof(key_cmd)) < 0) {
        fprintf(stderr, "chip_erase: key write failed\n");
        return -1;
    }

    /* 2. Verify CHIPERASE bit latched in ASI_KEY_STATUS (bit 3) */
    ks = updi_ldcs(fd, ASI_KEY_STATUS);
    if (ks < 0) {
        fprintf(stderr, "chip_erase: ldcs KEY_STATUS failed\n");
        return -1;
    }
    if (!(ks & 0x08u)) {
        fprintf(stderr,
                "chip_erase: CHIPERASE key not latched (KEY_STATUS=0x%02x)\n",
                ks & 0xFF);
        return -1;
    }

    /* 3. Assert system reset, then clear it */
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0) {
        fprintf(stderr, "chip_erase: reset assert failed\n");
        return -1;
    }
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0) {
        fprintf(stderr, "chip_erase: reset clear failed\n");
        return -1;
    }

    /* 4. Poll ASI_SYS_STATUS until LOCKSTATUS (bit 1) clears.
     *    Worst-case chip-erase on AVR128DA is well under 1 s.            */
    for (int i = 0; i < 2000; i++) {
        int s = updi_ldcs(fd, ASI_SYS_STATUS);
        if (s < 0) {
            fprintf(stderr, "chip_erase: ldcs SYS_STATUS failed\n");
            return -1;
        }
        if (!(s & 0x02u))
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "chip_erase: LOCKSTATUS did not clear within 2 s\n");
    return -1;
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

/* ── Device-signature diagnostics (Phase 7, LLR-UPDI-13) ──────────────── *
 *
 * updi_read_device_info() reads:
 *   - SIGROW.DEVICEID0..2 @ 0x1100-0x1102   (datasheet §7.6.1, Table 7-4)
 *   - SYSCFG.REVID        @ 0x0F01          (datasheet §8.3.2.1; SYSCFG
 *                                            base 0x0F00 per memory map)
 *   - SIGROW.SERNUM0..15  @ 0x1110-0x111F   (datasheet §7.6.2.3: 16 bytes)
 *   - ASI_SYS_STATUS, ASI_KEY_STATUS, ASI_STATUSB via LDCS
 *
 * The operation is non-destructive: it does not halt the CPU and does
 * not initiate any NVM activity. On the first UPDI read failure it sets
 * info->fail_op to a short ASCII tag identifying the failed step and
 * returns -1.                                                             */
int updi_read_device_info(int fd, UpdiDeviceInfo *info)
{
    int v;

    if (info == NULL)
        return -1;

    info->fail_op    = NULL;
    info->fail_errno = 0;

    if (updi_mem_read(fd, 0x1100u, info->device_id, 3u) < 0) {
        info->fail_op    = "sigrow";
        info->fail_errno = -1;
        return -1;
    }
    if (updi_mem_read(fd, 0x0F01u, &info->revid, 1u) < 0) {
        info->fail_op    = "revid";
        info->fail_errno = -1;
        return -1;
    }
    if (updi_mem_read(fd, 0x1110u, info->serial, 16u) < 0) {
        info->fail_op    = "sernum";
        info->fail_errno = -1;
        return -1;
    }

    v = updi_ldcs(fd, ASI_SYS_STATUS);
    if (v < 0) {
        info->fail_op    = "sys-status";
        info->fail_errno = -1;
        return -1;
    }
    info->asi_sys_status = (uint8_t)v;

    v = updi_ldcs(fd, ASI_KEY_STATUS);
    if (v < 0) {
        info->fail_op    = "key-status";
        info->fail_errno = -1;
        return -1;
    }
    info->asi_key_status = (uint8_t)v;

    v = updi_ldcs(fd, ASI_STATUSB);
    if (v < 0) {
        info->fail_op    = "asi-statusb";
        info->fail_errno = -1;
        return -1;
    }
    info->asi_statusb = (uint8_t)v;

    return 0;
}
