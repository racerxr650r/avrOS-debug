/* src/gdb_rsp.c — GDB Remote Serial Protocol server.
 *
 * Packet framing: $<payload>#<XX>. Checksum is XOR of every payload byte.
 * After QStartNoAckMode is negotiated, +/- ack bytes are not exchanged.
 */
#include "gdb_rsp.h"
#include "monitor.h"
#include "updi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ── Static buffers ──────────────────────────────────────────────────── */

static char   rsp_buf[RSP_PACKET_MAX + 8];

static bool g_noack = false;

void rsp_set_noack(bool e) { g_noack = e; }
bool rsp_get_noack(void)   { return g_noack; }

/* GDB AVR address-space split: addresses < 0x800000 are program memory
 * (FLASH); addresses >= 0x800000 are data space (SRAM/IO).  AVR-Dx UPDI
 * uses the opposite convention — FLASH lives at UPDI 0x800000+ and SRAM
 * at UPDI 0x000000+.  Translate by flipping bit 23.                    */
#define GDB_AVR_DATA_FLAG   0x800000u
#define GDB_AVR_ADDR_MASK   0x7FFFFFu

/* Sentinel: no breakpoint installed in this HW comparator slot. */
#define HW_BP_SLOT_EMPTY    0xFFFFFFFFu

/* ── small helpers ───────────────────────────────────────────────────── */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void byte_to_hex(uint8_t b, char *out)
{
    static const char hex[] = "0123456789abcdef";
    out[0] = hex[(b >> 4) & 0xFu];
    out[1] = hex[b & 0xFu];
}

static int parse_hex_u32(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    uint32_t v = 0;
    int n = 0;
    while (*p) {
        int d = hex_nibble(*p);
        if (d < 0) break;
        v = (v << 4) | (uint32_t)d;
        ++p; ++n;
    }
    if (n == 0) return -1;
    *out = v;
    *pp = p;
    return n;
}

static ssize_t write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n; left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int rsp_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int rsp_accept(int listen_fd)
{
    struct sockaddr_in peer;
    socklen_t slen = sizeof peer;
    int cfd;
    do {
        cfd = accept(listen_fd, (struct sockaddr *)&peer, &slen);
    } while (cfd < 0 && errno == EINTR);
    if (cfd < 0) return -1;

    int one = 1;
    (void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    /* Each new GDB session starts in ack mode and must explicitly
     * negotiate QStartNoAckMode again.  Without this reset, a second
     * client inherits the previous session's no-ack state, causing
     * gdb to retry qSupported (no '+' arrives) and eventually parse
     * its own internal "timeout" token as a feature item.            */
    g_noack = false;
    return cfd;
}

void rsp_close(int fd)
{
    if (fd >= 0) (void)close(fd);
}

/* ── Packet codec ────────────────────────────────────────────────────── */

static int read_one(int fd, char *c)
{
    ssize_t n;
    do { n = read(fd, c, 1); } while (n < 0 && errno == EINTR);
    if (n == 0) return 0;
    if (n < 0) return -1;
    return 1;
}

int rsp_recv_packet(int fd, char *buf, size_t cap)
{
    char c;
    int rc;

    /* skip stream until '$' */
    for (;;) {
        rc = read_one(fd, &c);
        if (rc == 0) return 0;
        if (rc < 0) return -1;
        if (c == '$') break;
        /* discard +, -, or stray bytes */
    }

    size_t plen = 0;
    uint8_t csum = 0;
    for (;;) {
        rc = read_one(fd, &c);
        if (rc <= 0) return rc < 0 ? -1 : 0;
        if (c == '#') break;
        if (plen + 1 >= cap) return -1;
        buf[plen++] = c;
        csum = (uint8_t)(csum + (uint8_t)c);
    }
    buf[plen] = '\0';

    char hh[2];
    if (read_one(fd, &hh[0]) <= 0) return -1;
    if (read_one(fd, &hh[1]) <= 0) return -1;
    int hi = hex_nibble(hh[0]);
    int lo = hex_nibble(hh[1]);
    if (hi < 0 || lo < 0) return -1;
    uint8_t got = (uint8_t)((hi << 4) | lo);

    if (g_noack) {
        return (got == csum) ? (int)plen : -1;
    }
    char ack = (got == csum) ? '+' : '-';
    if (write_all(fd, &ack, 1) < 0) return -1;
    return (got == csum) ? (int)plen : -1;
}

int rsp_send_packet(int fd, const char *payload)
{
    size_t plen = strlen(payload);
    if (plen + 4u > sizeof rsp_buf) return -1;
    uint8_t csum = 0;
    for (size_t i = 0; i < plen; ++i) csum = (uint8_t)(csum + (uint8_t)payload[i]);
    rsp_buf[0] = '$';
    memcpy(rsp_buf + 1, payload, plen);
    rsp_buf[1 + plen] = '#';
    byte_to_hex(csum, &rsp_buf[2 + plen]);
    return (write_all(fd, rsp_buf, plen + 4u) < 0) ? -1 : 0;
}

/* ── Default handlers ────────────────────────────────────────────────── */

/* Map an OCD halt-status reading to a GDB stop-signal string.
 * EXTBRK (host-issued STOP via OCD CTRLA, or external break pin)
 * presents as SIGINT; everything else (HW BP, SW BP, JMP/INT trap,
 * step completion, reset) presents as SIGTRAP — GDB's expected
 * default after `?`.                                                  */
static const char *signal_for_halt_status(int updi_fd)
{
    uint8_t st0 = 0, st1 = 0;
    if (updi_ocd_read_halt_status(updi_fd, &st0, &st1) < 0)
        return "T05";
    if (st1 & OCD_STATUS1_EXTBRK) return "T02";
    return "T05";
}

/* Clear all OCD HW breakpoints in silicon and reset the local shadow.
 * Idempotent and tolerant of UPDI errors (used on detach).            */
void rsp_hw_bp_clear_all(RspContext *ctx)
{
    for (int i = 0; i < 2; i++) {
        if (ctx->hw_bp_addr[i] != HW_BP_SLOT_EMPTY) {
            (void)updi_ocd_clear_hw_bp(ctx->updi_fd, i);
            ctx->hw_bp_addr[i] = HW_BP_SLOT_EMPTY;
        }
    }
}

/* HLR-056: clear all OCD data-address watchpoints in silicon and
 * reset the local shadow.  Idempotent; used on detach.               */
void rsp_hw_wp_clear_all(RspContext *ctx)
{
    for (int i = 0; i < 2; i++) {
        if (ctx->hw_wp[i].kind != '\0') {
            (void)updi_ocd_clear_data_bp(ctx->updi_fd, i);
            ctx->hw_wp[i].kind   = '\0';
            ctx->hw_wp[i].addr   = 0;
            ctx->hw_wp[i].length = 0;
        }
    }
}

#define hw_bp_clear_all rsp_hw_bp_clear_all
#define hw_wp_clear_all rsp_hw_wp_clear_all

/* HLR-056: scan halt-status for a data-watchpoint trigger and, if
 * found, append the GDB stop-key (`watch:`, `rwatch:`, or `awatch:`)
 * followed by the GDB-side address.  Idempotent — writes nothing
 * when no DABP* bit is set or no shadow slot matches.                */
static void append_watch_suffix(RspContext *ctx, char *dst, size_t cap)
{
    uint8_t st0 = 0, st1 = 0;
    if (updi_ocd_read_halt_status(ctx->updi_fd, &st0, &st1) < 0) return;
    int slot = -1;
    if      (st1 & OCD_STATUS1_DABP0) slot = 0;
    else if (st1 & OCD_STATUS1_DABP1) slot = 1;
    if (slot < 0) return;
    if (ctx->hw_wp[slot].kind == '\0') return;
    const char *key = "watch";
    switch (ctx->hw_wp[slot].kind) {
        case 'r': key = "rwatch"; break;
        case 'a': key = "awatch"; break;
        case 'w': default: key = "watch"; break;
    }
    size_t n = strlen(dst);
    if (n >= cap) return;
    (void)snprintf(dst + n, cap - n, "%s:%x;", key,
                   (unsigned)ctx->hw_wp[slot].addr);
}

/* These handlers each call rsp_send_packet() exactly once. */

static int reply_ok(int fd) { return rsp_send_packet(fd, "OK"); }
static int reply_empty(int fd) { return rsp_send_packet(fd, ""); }
static int reply_err(int fd, const char *code) { return rsp_send_packet(fd, code); }

static int dh_halt_reason(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    int aid = ctx->fsm ? fsm_get_active_thread(ctx->fsm) : 0;
    if (aid <= 0) aid = 1;
    char reply[64];
    snprintf(reply, sizeof reply, "%sthread:%x;",
             signal_for_halt_status(ctx->updi_fd), (unsigned)aid);
    append_watch_suffix(ctx, reply, sizeof reply);
    return rsp_send_packet(fd, reply);
}

/* Hex-encode one byte into *p (advances p by 2 chars). */
static void rsp_hex_byte(char **p, uint8_t v)
{
    static const char H[] = "0123456789abcdef";
    *(*p)++ = H[(v >> 4) & 0xFu];
    *(*p)++ = H[v & 0xFu];
}

/* Read live AVR CPU state via OCD into a 39-byte GDB AVR register block:
 *   r0..r31 (32 bytes) | SREG (1) | SPL (1) | SPH (1) | PC (4 LE)
 * = 78 hex chars.  Returns 0 on success, -1 on UPDI error.              */
static int ocd_read_avr_regblock(int updi_fd, char *out_hex)
{
    uint8_t  gpr[32];
    uint8_t  sreg = 0;
    uint16_t sp = 0;
    uint32_t pc_byte = 0;
    for (uint8_t i = 0; i < 32u; i++) {
        if (updi_ocd_read_gpr(updi_fd, i, &gpr[i]) < 0) return -1;
    }
    if (updi_ocd_read_sreg(updi_fd, &sreg)   < 0) return -1;
    if (updi_ocd_read_sp  (updi_fd, &sp)     < 0) return -1;
    if (updi_ocd_read_pc  (updi_fd, &pc_byte) < 0) return -1;

    char *p = out_hex;
    for (int i = 0; i < 32; i++) rsp_hex_byte(&p, gpr[i]);
    rsp_hex_byte(&p, sreg);
    rsp_hex_byte(&p, (uint8_t)(sp & 0xFFu));
    rsp_hex_byte(&p, (uint8_t)((sp >> 8) & 0xFFu));
    rsp_hex_byte(&p, (uint8_t)( pc_byte        & 0xFFu));
    rsp_hex_byte(&p, (uint8_t)((pc_byte >>  8) & 0xFFu));
    rsp_hex_byte(&p, (uint8_t)((pc_byte >> 16) & 0xFFu));
    rsp_hex_byte(&p, (uint8_t)((pc_byte >> 24) & 0xFFu));
    *p = '\0';
    return 0;
}

static int dh_read_regs(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    int tid = ctx->g_thread_p ? *ctx->g_thread_p : 0;
    int aid = ctx->fsm ? fsm_get_active_thread(ctx->fsm) : 0;
    if (tid <= 0) tid = aid;
    if (tid <= 0) tid = 1;

    /* Active thread → live CPU regs via OCD; non-active threads → the
     * FSM-reconstructed view (PC = saved state_fn, R-file zeros).
     * When no FSM thread is active (e.g. --load without --device), we
     * still want live CPU state from OCD rather than synthetic zeros.  */
    char reg[80] = {0};
    if (aid <= 0 || tid == aid) {
        if (ocd_read_avr_regblock(ctx->updi_fd, reg) < 0)
            return reply_err(fd, "E01");
        return rsp_send_packet(fd, reg);
    }
    if (fsm_get_registers(ctx->fsm, tid, reg) < 0)
        return reply_err(fd, "E01");
    return rsp_send_packet(fd, reg);
}

/* Route a single GDB AVR register slot to its OCD writer.
 *   regnum 0..31  : r0..r31 (1 byte)
 *   regnum 32     : SREG    (1 byte)
 *   regnum 33     : SP      (2 bytes, little-endian SPL|SPH)
 *   regnum 34     : PC      (4 bytes, little-endian byte address)
 */
static int ocd_write_avr_reg(int updi_fd, uint32_t regnum,
                             const uint8_t *bytes, size_t nb)
{
    if (regnum < 32u) {
        if (nb < 1u) return -1;
        return updi_ocd_write_gpr(updi_fd, (uint8_t)regnum, bytes[0]);
    }
    if (regnum == 32u) {
        if (nb < 1u) return -1;
        return updi_ocd_write_sreg(updi_fd, bytes[0]);
    }
    if (regnum == 33u) {
        if (nb < 2u) return -1;
        uint16_t sp = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
        return updi_ocd_write_sp(updi_fd, sp);
    }
    if (regnum == 34u) {
        if (nb < 4u) return -1;
        uint32_t pc = (uint32_t)bytes[0]
                    | ((uint32_t)bytes[1] << 8)
                    | ((uint32_t)bytes[2] << 16)
                    | ((uint32_t)bytes[3] << 24);
        return updi_ocd_write_pc(updi_fd, pc);
    }
    return -1; /* unknown reg */
}

static int dh_write_regs(int fd, const char *pkt, void *vctx)
{
    /* Accepts both Gxx... and Pn=xx... */
    RspContext *ctx = (RspContext *)vctx;
    if (pkt[0] == 'P') {
        /* P<reg>=<hex...> */
        const char *p = pkt + 1;
        uint32_t regnum;
        if (parse_hex_u32(&p, &regnum) < 0) return reply_err(fd, "E01");
        if (*p != '=') return reply_err(fd, "E01");
        ++p;
        size_t nibbles = strlen(p);
        if ((nibbles & 1u) != 0u) return reply_err(fd, "E01");
        uint8_t bytes[8];
        size_t nb = nibbles / 2u;
        if (nb > sizeof bytes) return reply_err(fd, "E01");
        for (size_t i = 0; i < nb; ++i) {
            int hi = hex_nibble(p[i * 2]);
            int lo = hex_nibble(p[i * 2 + 1]);
            if (hi < 0 || lo < 0) return reply_err(fd, "E01");
            bytes[i] = (uint8_t)((hi << 4) | lo);
        }
        if (ocd_write_avr_reg(ctx->updi_fd, regnum, bytes, nb) < 0) {
            return reply_err(fd, "E01");
        }
        return reply_ok(fd);
    }
    /* G<hex...> — full 39-byte AVR reg block */
    const char *p = pkt + 1;
    size_t nibbles = strlen(p);
    if (nibbles < 78u || (nibbles & 1u)) return reply_err(fd, "E01");
    uint8_t bytes[39];
    for (size_t i = 0; i < sizeof bytes; ++i) {
        int hi = hex_nibble(p[i * 2]);
        int lo = hex_nibble(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) return reply_err(fd, "E01");
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    /* r0..r31 */
    for (uint8_t n = 0; n < 32u; ++n) {
        if (updi_ocd_write_gpr(ctx->updi_fd, n, bytes[n]) < 0)
            return reply_err(fd, "E01");
    }
    /* SREG */
    if (updi_ocd_write_sreg(ctx->updi_fd, bytes[32]) < 0)
        return reply_err(fd, "E01");
    /* SP (LE) */
    {
        uint16_t sp = (uint16_t)(bytes[33] | ((uint16_t)bytes[34] << 8));
        if (updi_ocd_write_sp(ctx->updi_fd, sp) < 0)
            return reply_err(fd, "E01");
    }
    /* PC (LE, 4 bytes, byte address) */
    {
        uint32_t pc = (uint32_t)bytes[35]
                    | ((uint32_t)bytes[36] << 8)
                    | ((uint32_t)bytes[37] << 16)
                    | ((uint32_t)bytes[38] << 24);
        if (updi_ocd_write_pc(ctx->updi_fd, pc) < 0)
            return reply_err(fd, "E01");
    }
    return reply_ok(fd);
}

static int dh_read_mem(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    const char *p = pkt + 1;
    uint32_t addr, len;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    if (parse_hex_u32(&p, &len) < 0) return reply_err(fd, "E01");
    if (len == 0 || len > 512u) return reply_err(fd, "E01");

    /* GDB AVR memory map: 0x000000-0x7FFFFF = FLASH (program memory),
     * 0x800000+ = SRAM/IO (data memory).  AVR-Dx UPDI memory map: FLASH at
     * UPDI 0x800000+, SRAM/IO at UPDI 0x000000+.  Swap the bit-23 sense to
     * translate between the two spaces. */
    uint32_t updi_addr = (addr & GDB_AVR_DATA_FLAG)
                       ? (addr & GDB_AVR_ADDR_MASK)
                       : (addr | UPDI_FLASH_BASE);
    uint8_t buf[512];
    if (updi_mem_read(ctx->updi_fd, updi_addr, buf, len) < 0) {
        return reply_err(fd, "E01");
    }
    char reply[1025];
    for (uint32_t i = 0; i < len; ++i) byte_to_hex(buf[i], &reply[i * 2]);
    reply[len * 2] = '\0';
    return rsp_send_packet(fd, reply);
}

static int dh_write_mem(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    char op = pkt[0];   /* 'M' or 'X' */
    const char *p = pkt + 1;
    uint32_t addr, len;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    if (parse_hex_u32(&p, &len) < 0) return reply_err(fd, "E01");
    if (*p != ':') return reply_err(fd, "E01");
    ++p;
    if (len == 0 || len > 512u) return reply_err(fd, "E01");

    uint8_t data[512];
    if (op == 'M') {
        for (uint32_t i = 0; i < len; ++i) {
            int hi = hex_nibble(p[i * 2]);
            int lo = hex_nibble(p[i * 2 + 1]);
            if (hi < 0 || lo < 0) return reply_err(fd, "E01");
            data[i] = (uint8_t)((hi << 4) | lo);
        }
    } else { /* X — binary; assume already unescaped for our purposes */
        memcpy(data, p, len);
    }

    /* GDB AVR memory map: 0x000000-0x7FFFFF = FLASH, 0x800000+ = SRAM/IO.
     * AVR-Dx UPDI memory map: FLASH at UPDI 0x800000+, SRAM at UPDI 0x4000+.
     * SRAM writes go via mem_write (low UPDI addr); FLASH writes go via
     * the NVM controller using the UPDI 24-bit FLASH-base address. */
    int rc;
    if (addr & GDB_AVR_DATA_FLAG) {
        rc = updi_mem_write(ctx->updi_fd, addr & GDB_AVR_ADDR_MASK, data, len);
    } else {
        rc = updi_nvm_write_flash(ctx->updi_fd, addr | UPDI_FLASH_BASE,
                                  data, len);
    }
    return (rc < 0) ? reply_err(fd, "E01") : reply_ok(fd);
}

static int dh_continue(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;

    if (updi_run(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    fsm_invalidate(ctx->fsm);

    /* Wait for either:
     *   (a) the CPU to halt by itself (HW breakpoint, BREAK opcode,
     *       JMP-trap, etc.) — detected by polling ASI_OCD_STATUS, or
     *   (b) the gdb client sending a Ctrl-C (\x03) byte — user
     *       interrupt or `interrupt` command in gdb.
     *
     * Poll once per 5 ms.  This is well below human latency on (b)
     * yet adds only ~200 transactions/s on the UPDI link.  Tolerate up
     * to UPDI_FAIL_MAX consecutive UPDI poll failures before giving up
     * — protects against an infinite spin if the target loses power or
     * the serial link is yanked mid-run.                              */
    bool got_ctrl_c = false;
    enum { UPDI_FAIL_MAX = 8 };
    int  updi_fails = 0;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 0, 5 * 1000 };  /* 5 ms */
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            (void)updi_halt(ctx->updi_fd);
            return -1;
        }
        if (sel > 0 && FD_ISSET(fd, &rfds)) {
            char c;
            ssize_t n;
            do { n = read(fd, &c, 1); } while (n < 0 && errno == EINTR);
            if (n <= 0) {
                (void)updi_halt(ctx->updi_fd);
                return -1;
            }
            if (c == '\x03') {
                got_ctrl_c = true;
                break;
            }
            /* Liberal: discard any other stray bytes mid-run. */
            continue;
        }
        /* select() timed out — probe OCD STATUS for spontaneous halt.
         *   updi_ocd_poll_halted() returns:
         *     0  = CPU halted (we're done)
         *     1  = link OK, still running (keep polling)
         *    -1  = UPDI I/O error (count toward UPDI_FAIL_MAX)        */
        int s = updi_ocd_poll_halted(ctx->updi_fd, 1);
        if (s == 0) { updi_fails = 0; break; }       /* halted */
        if (s > 0)  { updi_fails = 0; continue; }    /* still running */
        if (++updi_fails >= UPDI_FAIL_MAX) {
            (void)updi_halt(ctx->updi_fd);
            return reply_err(fd, "E01");
        }
    }

    if (updi_halt(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    fsm_invalidate(ctx->fsm);
    if (ctx->fsm) {
        (void)fsm_build_thread_list(ctx->fsm, ctx->idx, ctx->updi_fd);
    }
    /* Signal: SIGINT (T02) for user-Ctrl-C, SIGTRAP (T05) otherwise. */
    {
        int aid = ctx->fsm ? fsm_get_active_thread(ctx->fsm) : 0;
        if (aid <= 0) aid = 1;
        const char *sig = got_ctrl_c ? "T02" : "T05";
        char reply[64];
        snprintf(reply, sizeof reply, "%sthread:%x;", sig, (unsigned)aid);
        append_watch_suffix(ctx, reply, sizeof reply);
        return rsp_send_packet(fd, reply);
    }
}

static int dh_step(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    if (updi_step(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    fsm_invalidate(ctx->fsm);
    if (ctx->fsm) {
        (void)fsm_build_thread_list(ctx->fsm, ctx->idx, ctx->updi_fd);
    }
    return dh_halt_reason(fd, "?", vctx);
}

/* AVR-Dx OCD provides exactly two hardware breakpoint comparators
 * (BP0, BP1).  The per-session shadow lives in RspContext so detach
 * and reattach cycles leave silicon in a known state.                */

static int dh_insert_bp(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    /* Z0 = software breakpoint, Z1 = hardware breakpoint.
     *
     * Z0 in OCD mode would normally be implemented by patching FLASH
     * with the AVR BREAK opcode.  That requires exiting OCD, entering
     * NVMPROG (which issues a system-reset pulse), patching the page,
     * then re-entering OCD — a sequence that destroys live CPU state
     * (PC/SREG/GPRs) and is non-trivial to save/restore over UPDI.
     *
     * Pragmatic choice: route Z0 to the same two HW comparators so
     * plain `break` Just Works up to two simultaneous breakpoints,
     * matching what microchip-pic-avr-tools / pyedbglib do for the
     * same silicon.  When all slots are full we return E08 so GDB
     * surfaces the limit to the user.                                */
    char kind = pkt[1];
    /* HLR-056: Z2/Z3/Z4 — data watchpoints (write/read/access). */
    if (kind == '2' || kind == '3' || kind == '4') {
        const char *p = pkt + 2;
        if (*p != ',') return reply_err(fd, "E01");
        ++p;
        uint32_t addr;
        if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");
        uint32_t length = 1;
        if (*p == ',') { ++p; if (parse_hex_u32(&p, &length) < 0) return reply_err(fd, "E01"); }
        char wkind = (kind == '2') ? 'w' : (kind == '3') ? 'r' : 'a';
        /* Idempotent re-insert on same (addr,kind): reply OK. */
        for (int i = 0; i < 2; i++) {
            if (ctx->hw_wp[i].kind != '\0' &&
                ctx->hw_wp[i].addr == addr &&
                ctx->hw_wp[i].kind == wkind) {
                return reply_ok(fd);
            }
        }
        int slot_i = -1;
        for (int i = 0; i < 2; i++) {
            if (ctx->hw_wp[i].kind == '\0') { slot_i = i; break; }
        }
        if (slot_i < 0) return reply_err(fd, "E08");      /* both slots used */
        /* Strip the GDB data-space flag (0x800000) for the silicon. */
        uint32_t data_byte = addr & GDB_AVR_ADDR_MASK;
        if (updi_ocd_set_data_bp(ctx->updi_fd, slot_i, data_byte,
                                 (uint8_t)length, wkind) < 0)
            return reply_err(fd, "E01");
        ctx->hw_wp[slot_i].addr   = addr;
        ctx->hw_wp[slot_i].length = (uint8_t)length;
        ctx->hw_wp[slot_i].kind   = wkind;
        return reply_ok(fd);
    }
    if (kind != '0' && kind != '1') return reply_empty(fd);
    const char *p = pkt + 2;
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    uint32_t addr;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");

    /* Already set?  Idempotent OK. */
    for (int i = 0; i < 2; i++) {
        if (ctx->hw_bp_addr[i] == addr) return reply_ok(fd);
    }
    int slot_i = -1;
    for (int i = 0; i < 2; i++) {
        if (ctx->hw_bp_addr[i] == HW_BP_SLOT_EMPTY) { slot_i = i; break; }
    }
    if (slot_i < 0) return reply_err(fd, "E08");          /* both slots used */
    uint32_t flash_byte = addr & GDB_AVR_ADDR_MASK;       /* gdb→byte */
    if (updi_ocd_set_hw_bp(ctx->updi_fd, slot_i, flash_byte) < 0)
        return reply_err(fd, "E01");
    ctx->hw_bp_addr[slot_i] = addr;
    return reply_ok(fd);
}

static int dh_remove_bp(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    char kind = pkt[1];
    /* HLR-056: z2/z3/z4 — data watchpoints. */
    if (kind == '2' || kind == '3' || kind == '4') {
        const char *p = pkt + 2;
        if (*p != ',') return reply_err(fd, "E01");
        ++p;
        uint32_t addr;
        if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");
        for (int i = 0; i < 2; i++) {
            if (ctx->hw_wp[i].kind != '\0' && ctx->hw_wp[i].addr == addr) {
                if (updi_ocd_clear_data_bp(ctx->updi_fd, i) < 0)
                    return reply_err(fd, "E01");
                ctx->hw_wp[i].kind   = '\0';
                ctx->hw_wp[i].addr   = 0;
                ctx->hw_wp[i].length = 0;
                return reply_ok(fd);
            }
        }
        return reply_ok(fd);   /* unknown — be tolerant for resync */
    }
    /* Z0 and Z1 both map to HW comparators (see dh_insert_bp). */
    if (kind != '0' && kind != '1') return reply_empty(fd);
    const char *p = pkt + 2;
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    uint32_t addr;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");

    for (int i = 0; i < 2; i++) {
        if (ctx->hw_bp_addr[i] == addr) {
            if (updi_ocd_clear_hw_bp(ctx->updi_fd, i) < 0)
                return reply_err(fd, "E01");
            ctx->hw_bp_addr[i] = HW_BP_SLOT_EMPTY;
            return reply_ok(fd);
        }
    }
    /* No record (e.g. server restarted mid-session) — report OK so a
     * GDB resync is non-fatal.                                       */
    return reply_ok(fd);
}

static int dh_thread_info(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    bool first = (strncmp(pkt, "qfThreadInfo", 12) == 0);
    if (!first) return rsp_send_packet(fd, "l");   /* end of list */

    char reply[256] = "m";
    size_t off = 1;
    int n = ctx->fsm ? ctx->fsm->thread_count : 0;
    for (int i = 0; i < n; ++i) {
        int written = snprintf(reply + off, sizeof reply - off,
                               "%s%x", (i == 0 ? "" : ","),
                               (unsigned)ctx->fsm->threads[i].gdb_id);
        if (written < 0 || (size_t)written >= sizeof reply - off) break;
        off += (size_t)written;
    }
    if (n == 0) return rsp_send_packet(fd, "l");
    return rsp_send_packet(fd, reply);
}

static int dh_thread_extra(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    const char *p = strchr(pkt, ',');
    if (p == NULL) return reply_err(fd, "E01");
    ++p;
    uint32_t tid;
    if (parse_hex_u32(&p, &tid) < 0) return reply_err(fd, "E01");

    const char *name = "<unknown>";
    if (ctx->fsm) {
        for (int i = 0; i < ctx->fsm->thread_count; ++i) {
            if ((uint32_t)ctx->fsm->threads[i].gdb_id == tid) {
                name = ctx->fsm->threads[i].name;
                break;
            }
        }
    }
    size_t nlen = strlen(name);
    char reply[80];
    if (nlen * 2u + 1u > sizeof reply) nlen = (sizeof reply - 1u) / 2u;
    for (size_t i = 0; i < nlen; ++i) byte_to_hex((uint8_t)name[i], &reply[i * 2]);
    reply[nlen * 2] = '\0';
    return rsp_send_packet(fd, reply);
}

static int parse_h_tid(const char *pkt, int *out_tid)
{
    /* Hg<tid> or Hc<tid>; tid is hex; -1 means all threads */
    const char *p = pkt + 2;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    uint32_t v;
    if (parse_hex_u32(&p, &v) < 0) return -1;
    *out_tid = neg ? -(int)v : (int)v;
    return 0;
}

static int dh_set_thread_g(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    int tid;
    if (parse_h_tid(pkt, &tid) < 0) return reply_err(fd, "E01");
    if (tid <= 0) tid = ctx->fsm ? fsm_get_active_thread(ctx->fsm) : 1;
    if (tid <= 0) tid = 1;
    if (ctx->g_thread_p) *ctx->g_thread_p = tid;
    return reply_ok(fd);
}

static int dh_set_thread_c(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    int tid;
    if (parse_h_tid(pkt, &tid) < 0) return reply_err(fd, "E01");
    if (tid <= 0) tid = ctx->fsm ? fsm_get_active_thread(ctx->fsm) : 1;
    if (tid <= 0) tid = 1;
    if (ctx->c_thread_p) *ctx->c_thread_p = tid;
    return reply_ok(fd);
}

static int dh_monitor(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    /* pkt is "qRcmd,<hex>" — pass <hex> as-is to monitor_dispatch */
    const char *p = strchr(pkt, ',');
    if (p == NULL) return reply_empty(fd);
    ++p;
    int rc = monitor_dispatch_ex(fd, ctx, p);
    if (rc == 0) return reply_ok(fd);
    if (rc == -3) {
        /* monitor verb already emitted its own packet (stop-reply or
         * E-packet) — do not send a trailing OK.                     */
        return 0;
    }
    if (rc == -1) {
        /* O-packet error message hex-encoded */
        static const char err[] = "monitor: UPDI failure\n";
        char o[2 + sizeof err * 2u];
        o[0] = 'O';
        size_t n = strlen(err);
        for (size_t i = 0; i < n; ++i) byte_to_hex((uint8_t)err[i], &o[1 + i * 2u]);
        o[1 + n * 2u] = '\0';
        (void)rsp_send_packet(fd, o);
        return reply_empty(fd);
    }
    return reply_empty(fd);
}

static int dh_detach(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    if (pkt[0] == 'k') {
        if (ctx->quit_p) *ctx->quit_p = 1;
        return reply_ok(fd);
    }
    /* D — release HW breakpoint comparators in silicon (otherwise the
     * next session inherits stale BPs from a different GDB process)
     * and resume the CPU before closing the GDB socket.              */
    hw_bp_clear_all(ctx);
    hw_wp_clear_all(ctx);
    (void)updi_run(ctx->updi_fd);
    (void)reply_ok(fd);
    if (ctx->gdb_fd_p) {
        if (*ctx->gdb_fd_p >= 0) rsp_close(*ctx->gdb_fd_p);
        *ctx->gdb_fd_p = -1;
    }
    return 0;
}

/* HLR-059 (LLR-RSP-19): qC — report the currently selected c-thread.
 * Reply "QC<hex tid>" using the c-thread selected by the most recent
 * Hc packet, falling back to "QC0" when no thread is selected.  No
 * target access.                                                       */
static int dh_query_c(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    int tid = (ctx->c_thread_p != NULL) ? *ctx->c_thread_p : 0;
    if (tid <= 0) return rsp_send_packet(fd, "QC0");
    char reply[16];
    (void)snprintf(reply, sizeof reply, "QC%x", (unsigned)tid);
    return rsp_send_packet(fd, reply);
}

/* HLR-059 (LLR-RSP-20): qOffsets — the AVR reset vector is fixed at
 * address zero and avrOS performs no position-independent loading, so
 * all three section offsets are reported as zero unconditionally. No
 * target access.                                                       */
static int dh_query_offsets(int fd, const char *pkt, void *vctx)
{
    (void)pkt; (void)vctx;
    return rsp_send_packet(fd, "Text=0;Data=0;Bss=0");
}

/* HLR-059 (LLR-RSP-21): T<tid> — is-thread-alive.  Reply OK when the
 * decoded thread id is present in FsmContext.threads[], else E01. When
 * the FSM context has not yet been built the active thread id (1) is
 * the only live tid; this mirrors the default empty thread list. */
static int dh_thread_alive(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    const char *p = pkt + 1;            /* skip 'T' */
    uint32_t tid = 0;
    if (parse_hex_u32(&p, &tid) < 0) return reply_err(fd, "E01");
    if (ctx->fsm != NULL && ctx->fsm->valid) {
        for (int i = 0; i < ctx->fsm->thread_count; ++i) {
            if ((uint32_t)ctx->fsm->threads[i].gdb_id == tid) {
                return reply_ok(fd);
            }
        }
        return reply_err(fd, "E01");
    }
    /* No valid FSM context — accept the default thread id 1. */
    return (tid == 1u) ? reply_ok(fd) : reply_err(fd, "E01");
}

/* HLR-059 (LLR-RSP-22): R<XX> — extended-remote restart.  Behaves as
 * "monitor reset" (CPU reset, leave halted in OCD) followed by a "c"
 * (run, poll for halt, emit a stop packet).  The <XX> byte is ignored
 * per the GDB protocol — it is present only to keep the packet length
 * predictable.                                                         */
static int dh_restart(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;

    /* Reset the target and re-enter OCD halted at the reset vector.   */
    if (updi_enter_debug(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    /* Invalidate FSM cache — post-reset state must be re-read.         */
    fsm_invalidate(ctx->fsm);
    /* Continue execution; dh_continue() will rebuild the thread list
     * after the next halt and emit the stop reply.                     */
    return dh_continue(fd, "c", vctx);
}

/* HLR-058 (LLR-RSP-23): vRun;[<filename>][;<arg>...] — extended-remote
 * start-new-program.  avr-updi-gdb has no filesystem on the target and
 * the program is already in FLASH, so all arguments are ignored.  The
 * behaviour is identical to R<XX>: reset the AVR, invalidate the FSM
 * cache, then continue and emit a stop reply when the target halts.    */
static int dh_vrun(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;

    if (updi_enter_debug(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    fsm_invalidate(ctx->fsm);
    return dh_continue(fd, "c", vctx);
}

/* HLR-058 (LLR-RSP-24): vAttach;<pid> — extended-remote attach.  The
 * single-process AVR target has no real pid namespace, so the <pid>
 * field is ignored.  Drop into OCD (halts a running target, no-op for
 * an already-halted target), invalidate the FSM cache, rebuild the
 * thread list, and emit a SIGTRAP stop reply so GDB knows the target
 * is now halted and ready for register/memory queries.                 */
static int dh_vattach(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;

    if (updi_enter_debug(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    fsm_invalidate(ctx->fsm);
    (void)fsm_build_thread_list(ctx->fsm, ctx->idx, ctx->updi_fd);
    return dh_halt_reason(fd, "?", vctx);
}

/* HLR-058 (LLR-RSP-25): vKill[;<pid>] — extended-remote kill.  Reply
 * "OK" and set the quit flag so the main loop terminates after the
 * current packet, mirroring the legacy `k` packet path.                */
static int dh_vkill(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    if (ctx->quit_p) *ctx->quit_p = 1;
    return reply_ok(fd);
}

void rsp_default_handlers(RspHandlers *h, RspContext *ctx)
{
    /* Mark both HW BP comparators empty.  Caller may have memset()
     * RspContext to zero, but the slot-empty sentinel is 0xFFFFFFFF.  */
    ctx->hw_bp_addr[0] = HW_BP_SLOT_EMPTY;
    ctx->hw_bp_addr[1] = HW_BP_SLOT_EMPTY;
    /* HLR-056: mark both DABP watchpoint comparators empty (kind='\0'). */
    ctx->hw_wp[0].kind = '\0';
    ctx->hw_wp[1].kind = '\0';

    h->on_halt_reason  = dh_halt_reason;
    h->on_read_regs    = dh_read_regs;
    h->on_write_regs   = dh_write_regs;
    h->on_read_mem     = dh_read_mem;
    h->on_write_mem    = dh_write_mem;
    h->on_continue     = dh_continue;
    h->on_step         = dh_step;
    h->on_insert_bp    = dh_insert_bp;
    h->on_remove_bp    = dh_remove_bp;
    h->on_thread_info  = dh_thread_info;
    h->on_thread_extra = dh_thread_extra;
    h->on_set_thread_g = dh_set_thread_g;
    h->on_set_thread_c = dh_set_thread_c;
    h->on_monitor      = dh_monitor;
    h->on_detach       = dh_detach;
    h->on_query_c      = dh_query_c;
    h->on_query_offsets= dh_query_offsets;
    h->on_thread_alive = dh_thread_alive;
    h->on_restart      = dh_restart;
    h->on_vrun         = dh_vrun;
    h->on_vattach      = dh_vattach;
    h->on_vkill        = dh_vkill;
    h->ctx             = ctx;
}

/* ── Dispatch ────────────────────────────────────────────────────────── */

static int call_handler(RspHandlerFn fn, int fd, const char *pkt, void *ctx)
{
    if (fn == NULL) return reply_empty(fd);
    return fn(fd, pkt, ctx);
}

int rsp_dispatch(int fd, const char *packet, RspHandlers *h)
{
    if (packet == NULL || packet[0] == '\0') return reply_empty(fd);

    /* ── Inline-handled (no target access) ─────────────────────────── */
    if (strncmp(packet, "qSupported", 10) == 0) {
        return rsp_send_packet(fd,
            "PacketSize=800;QStartNoAckMode+;multiprocess+;vContSupported+"
            ";vRun+;vAttach+;vKill+");
    }
    if (strncmp(packet, "qAttached", 9) == 0) {
        return rsp_send_packet(fd, "1");
    }
    if (strncmp(packet, "QStartNoAckMode", 15) == 0) {
        rsp_set_noack(true);
        return reply_ok(fd);
    }
    if (strncmp(packet, "vCont?", 6) == 0) {
        return rsp_send_packet(fd, "vCont;c;s");
    }
    if (strncmp(packet, "vCont;c", 7) == 0) {
        return call_handler(h->on_continue, fd, packet, h->ctx);
    }
    if (strncmp(packet, "vCont;s", 7) == 0) {
        return call_handler(h->on_step, fd, packet, h->ctx);
    }
    if (strncmp(packet, "vRun", 4) == 0 &&
        (packet[4] == '\0' || packet[4] == ';')) {
        return call_handler(h->on_vrun, fd, packet, h->ctx);
    }
    if (strncmp(packet, "vAttach", 7) == 0 &&
        (packet[7] == '\0' || packet[7] == ';' || packet[7] == '?')) {
        if (packet[7] == '?') return reply_ok(fd);   /* vAttach? probe */
        return call_handler(h->on_vattach, fd, packet, h->ctx);
    }
    if (strncmp(packet, "vKill", 5) == 0 &&
        (packet[5] == '\0' || packet[5] == ';')) {
        return call_handler(h->on_vkill, fd, packet, h->ctx);
    }

    /* ── Dispatch by leading character ────────────────────────────── */
    switch (packet[0]) {
        case '?':  return call_handler(h->on_halt_reason,  fd, packet, h->ctx);
        case 'g':  return call_handler(h->on_read_regs,    fd, packet, h->ctx);
        case 'G':  return call_handler(h->on_write_regs,   fd, packet, h->ctx);
        case 'P':  return call_handler(h->on_write_regs,   fd, packet, h->ctx);
        case 'm':  return call_handler(h->on_read_mem,     fd, packet, h->ctx);
        case 'M':  return call_handler(h->on_write_mem,    fd, packet, h->ctx);
        case 'X':  return call_handler(h->on_write_mem,    fd, packet, h->ctx);
        case 'c':  return call_handler(h->on_continue,     fd, packet, h->ctx);
        case 's':  return call_handler(h->on_step,         fd, packet, h->ctx);
        case 'Z':  return call_handler(h->on_insert_bp,    fd, packet, h->ctx);
        case 'z':  return call_handler(h->on_remove_bp,    fd, packet, h->ctx);
        case 'D':  return call_handler(h->on_detach,       fd, packet, h->ctx);
        case 'k':  return call_handler(h->on_detach,       fd, packet, h->ctx);
        case 'T':  return call_handler(h->on_thread_alive, fd, packet, h->ctx);
        case 'R':  return call_handler(h->on_restart,      fd, packet, h->ctx);
        case 'H':
            if (packet[1] == 'g') return call_handler(h->on_set_thread_g, fd, packet, h->ctx);
            if (packet[1] == 'c') return call_handler(h->on_set_thread_c, fd, packet, h->ctx);
            return reply_empty(fd);
        case 'q':
            if (strncmp(packet, "qfThreadInfo", 12) == 0 ||
                strncmp(packet, "qsThreadInfo", 12) == 0) {
                return call_handler(h->on_thread_info, fd, packet, h->ctx);
            }
            if (strncmp(packet, "qThreadExtraInfo", 16) == 0) {
                return call_handler(h->on_thread_extra, fd, packet, h->ctx);
            }
            if (strncmp(packet, "qRcmd,", 6) == 0) {
                return call_handler(h->on_monitor, fd, packet, h->ctx);
            }
            if (strncmp(packet, "qOffsets", 8) == 0) {
                return call_handler(h->on_query_offsets, fd, packet, h->ctx);
            }
            /* qC must be matched after the longer qC* probes above
             * (none today; safe).  Match only "qC" exactly so we don't
             * shadow a future "qCRC" or similar.                       */
            if (packet[1] == 'C' && (packet[2] == '\0' || packet[2] == ';')) {
                return call_handler(h->on_query_c, fd, packet, h->ctx);
            }
            return reply_empty(fd);
        default:
            return reply_empty(fd);
    }
}
