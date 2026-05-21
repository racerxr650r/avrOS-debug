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

/* HLR-053: byte length of the most recently received packet.  Set by
 * rsp_dispatch_n(); read by binary handlers (vFlashWrite) that need the
 * true length because the payload may contain embedded NUL bytes that
 * would truncate strlen().  Defaults to 0 so handlers invoked by tests
 * via rsp_dispatch() compute their own length from strlen().          */
static size_t g_dispatch_packet_len = 0;

/* HLR-053: abort any in-progress vFlash* transaction.  Called by the
 * core target-access handlers (m/M/c/s/g/G) when GDB violates the
 * "no other packets between vFlashErase and vFlashDone" rule.         */
static void flash_xact_abort(RspContext *ctx)
{
    if (ctx == NULL) return;
    if (ctx->flash_xact_buf != NULL) {
        free(ctx->flash_xact_buf);
        ctx->flash_xact_buf = NULL;
    }
    ctx->flash_xact_base = 0;
    ctx->flash_xact_len  = 0;
}

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

/* HLR-060 / LLR-RSP-40: server-wide GDB process id used in every reply
 * that carries a `<pid>` field.  Fixed at compile time so quiescent and
 * synthesised frames reference a stable namespace.  Value historically
 * observed by GDB on the wire (avrOS run-time pid).                    */
#define RSP_PID  0xa410u

/* HLR-060 / LLR-RSP-40: parse a GDB thread-id field at *pp.
 *
 * Recognises five forms:
 *   - bare hex `<TID>`            → pid=RSP_PID, tid=parsed,    any=false
 *   - bare `0`                    → pid=RSP_PID, tid=0,         any=true
 *   - bare `-1`                   → pid=RSP_PID, tid=(uint32_t)-1, any=true
 *   - `p<PID>.<TID>`              → pid/tid as parsed, any=any field-is-0/-1
 *   - `p<PID>.0` / `p<PID>.-1`    → any=true
 *   - `p0.<TID>` / `p-1.<TID>`    → pid mapped to RSP_PID, any=true
 *
 * Returns 0 on success, -1 on syntax error.  On error the *_out pointers
 * are not modified.  Advances *pp past the parsed field on success.    */
static int parse_mp_thread_id(const char **pp,
                              uint32_t *pid_out,
                              uint32_t *tid_out,
                              bool *any_out)
{
    const char *p = *pp;
    uint32_t pid = RSP_PID;
    uint32_t tid = 0;
    bool any = false;
    bool pid_special = false;          /* pid = 0 or -1 */
    bool tid_special = false;

    if (*p == 'p') {
        /* Multiprocess form: p<PID>.<TID>. */
        ++p;
        if (*p == '-' && *(p + 1) == '1') {
            pid = RSP_PID;
            pid_special = true;
            p += 2;
        } else {
            uint32_t v;
            if (parse_hex_u32(&p, &v) < 0) return -1;
            if (v == 0u) { pid = RSP_PID; pid_special = true; }
            else         { pid = v; }
        }
        if (*p != '.') return -1;
        ++p;
        if (*p == '-' && *(p + 1) == '1') {
            tid = (uint32_t)-1;
            tid_special = true;
            p += 2;
        } else {
            uint32_t v;
            if (parse_hex_u32(&p, &v) < 0) return -1;
            tid = v;
            if (v == 0u) tid_special = true;
        }
        any = pid_special || tid_special;
    } else if (*p == '-') {
        /* Bare `-1` shorthand. */
        if (*(p + 1) != '1') return -1;
        tid = (uint32_t)-1;
        any = true;
        p += 2;
    } else {
        /* Bare hex tid. */
        uint32_t v;
        if (parse_hex_u32(&p, &v) < 0) return -1;
        tid = v;
        any = (v == 0u);
    }

    *pp = p;
    if (pid_out) *pid_out = pid;
    if (tid_out) *tid_out = tid;
    if (any_out) *any_out = any;
    return 0;
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

#define hw_bp_clear_all rsp_hw_bp_clear_all

/* HLR-054: drop every SW-BP shadow entry.  Callers (vFlashDone,
 * monitor reset, monitor chip-erase) have already destroyed the
 * underlying FLASH instruction, so no silicon I/O is needed.        */
void rsp_sw_bp_clear_all(RspContext *ctx)
{
    for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
        ctx->sw_bp[i].in_use = false;
        ctx->sw_bp[i].addr   = 0;
        ctx->sw_bp[i].orig[0] = 0;
        ctx->sw_bp[i].orig[1] = 0;
    }
}

/* HLR-054: AVR BREAK instruction (little-endian in FLASH).  Decoded
 * by the AVR core as an OCD-trapping breakpoint regardless of the
 * current debug-enable state — exactly the semantic GDB needs for a
 * software breakpoint.                                              */
static const uint8_t SW_BP_BREAK_BYTES[2] = { 0x98u, 0x95u };

/* Snapshot R0–R31, SREG, SP, and PC via the OCD register file.
 * Returns -1 on any UPDI link failure.                              */
static int sw_bp_snapshot_cpu(int updi_fd, uint8_t gpr[32],
                              uint8_t *sreg, uint16_t *sp, uint32_t *pc)
{
    for (int i = 0; i < 32; i++) {
        if (updi_ocd_read_gpr(updi_fd, (uint8_t)i, &gpr[i]) < 0) return -1;
    }
    if (updi_ocd_read_sreg(updi_fd, sreg) < 0) return -1;
    if (updi_ocd_read_sp  (updi_fd, sp)   < 0) return -1;
    if (updi_ocd_read_pc  (updi_fd, pc)   < 0) return -1;
    return 0;
}

/* Restore the register file after the NVMPROG → OCD round-trip.    */
static int sw_bp_restore_cpu(int updi_fd, const uint8_t gpr[32],
                             uint8_t sreg, uint16_t sp, uint32_t pc)
{
    for (int i = 0; i < 32; i++) {
        if (updi_ocd_write_gpr(updi_fd, (uint8_t)i, gpr[i]) < 0) return -1;
    }
    if (updi_ocd_write_sreg(updi_fd, sreg) < 0) return -1;
    if (updi_ocd_write_sp  (updi_fd, sp)   < 0) return -1;
    if (updi_ocd_write_pc  (updi_fd, pc)   < 0) return -1;
    return 0;
}

/* HLR-054: install or remove a software breakpoint by patching the
 * 2-byte FLASH word at `byte_addr` (UPDI-side address, no FLASH_BASE
 * offset).  When `orig_out` is non-NULL the current FLASH word is
 * read out first (used during Z0 install to capture the opcode that
 * z0 will later restore).  CPU state is snapshotted before the
 * NVMPROG entry that `updi_nvm_flash_patch()` performs internally
 * and restored after `updi_enter_debug()` brings the chip back into
 * OCD.  Returns 0 on success, -1 on any UPDI failure.               */
static int sw_bp_patch_flash(int updi_fd, uint32_t byte_addr,
                             const uint8_t bytes[2], uint8_t orig_out[2])
{
    uint8_t  gpr[32], sreg;
    uint16_t sp;
    uint32_t pc;

    if (sw_bp_snapshot_cpu(updi_fd, gpr, &sreg, &sp, &pc) < 0) return -1;

    if (orig_out != NULL) {
        if (updi_mem_read(updi_fd, UPDI_FLASH_BASE + byte_addr,
                          orig_out, 2) < 0)
            return -1;
    }

    if (updi_nvm_flash_patch(updi_fd, UPDI_FLASH_BASE + byte_addr,
                             bytes, 2) < 0)
        return -1;

    if (updi_enter_debug(updi_fd) < 0) return -1;

    return sw_bp_restore_cpu(updi_fd, gpr, sreg, sp, pc);
}

/* HLR-056: data-address watchpoints over UPDI are not implemented in
 * silicon.  See src/updi.h and doc/reference/guesswork.md.  The Z2/
 * Z3/Z4 dispatchers reply with the empty packet so GDB falls back to
 * software watchpoints; consequently there is no DABP* halt cause to
 * decode here.                                                       */

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
     * the NVM controller's read-modify-write path so that arbitrary
     * `M`/`X` chunks (which need not be page-aligned nor page-multiple,
     * e.g. those emitted by `(gdb) load` against a stub that does not
     * advertise qXfer:memory-map) succeed without the strict alignment
     * required by the bulk page programmer. `updi_nvm_flash_patch` exits
     * leaving the chip in NVMPROG, so re-enter OCD mode before reply
     * so any follow-up register/memory query sees a live CPU. */
    int rc;
    if (addr & GDB_AVR_DATA_FLAG) {
        rc = updi_mem_write(ctx->updi_fd, addr & GDB_AVR_ADDR_MASK, data, len);
    } else {
        rc = updi_nvm_flash_patch(ctx->updi_fd, addr | UPDI_FLASH_BASE,
                                  data, len);
        if (rc >= 0) (void)updi_enter_debug(ctx->updi_fd);
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
    /* HLR-056: Z2/Z3/Z4 — AVR-Dx OCD over UPDI exposes no data-address
     * watchpoint hardware (see src/updi.h, doc/reference/guesswork.md).
     * Reply with the empty packet so GDB transparently falls back to
     * software watchpoints.                                            */
    if (kind == '2' || kind == '3' || kind == '4') {
        return reply_empty(fd);
    }
    if (kind != '0' && kind != '1') return reply_empty(fd);
    const char *p = pkt + 2;
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    uint32_t addr;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");

    /* HLR-054: Z0 with bp_mode==SW patches the BREAK opcode into FLASH
     * for an unbounded number of simultaneous SW breakpoints.  Z1, or
     * Z0 in `hw-only` mode (HLR-055), still aliases to the two OCD HW
     * comparators preserved from the Phase 1–8 baseline (HLR-016).    */
    if (kind == '0' && ctx->bp_mode == RSP_BP_MODE_SW) {
        /* Reject addresses in the data-space windows (SIGROW, FUSES,
         * USERROW, EEPROM, LOCK) — `BREAK` is only meaningful when
         * fetched from FLASH as an instruction.                       */
        if (addr & GDB_AVR_DATA_FLAG) return reply_err(fd, "E22");
        /* Idempotent re-insert on the same address. */
        for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
            if (ctx->sw_bp[i].in_use && ctx->sw_bp[i].addr == addr)
                return reply_ok(fd);
        }
        int slot_i = -1;
        for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
            if (!ctx->sw_bp[i].in_use) { slot_i = i; break; }
        }
        if (slot_i < 0) return reply_err(fd, "E08");
        uint32_t byte_addr = addr & GDB_AVR_ADDR_MASK;
        uint8_t  orig[2];
        if (sw_bp_patch_flash(ctx->updi_fd, byte_addr,
                              SW_BP_BREAK_BYTES, orig) < 0)
            return reply_err(fd, "E01");
        ctx->sw_bp[slot_i].in_use  = true;
        ctx->sw_bp[slot_i].addr    = addr;
        ctx->sw_bp[slot_i].orig[0] = orig[0];
        ctx->sw_bp[slot_i].orig[1] = orig[1];
        return reply_ok(fd);
    }

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
    /* HLR-056: z2/z3/z4 — see dh_insert_bp.  Hardware not present. */
    if (kind == '2' || kind == '3' || kind == '4') {
        return reply_empty(fd);
    }
    /* Z0 and Z1 both map to HW comparators (see dh_insert_bp). */
    if (kind != '0' && kind != '1') return reply_empty(fd);
    const char *p = pkt + 2;
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    uint32_t addr;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");

    /* HLR-054: matching z0 in SW mode restores the captured original
     * opcode and frees the shadow slot.  If no shadow matches we fall
     * through to the HW-comparator path so a server that toggled
     * bp_mode mid-session still drains both shadow tables.            */
    if (kind == '0' && ctx->bp_mode == RSP_BP_MODE_SW) {
        for (int i = 0; i < RSP_MAX_SW_BREAKPOINTS; i++) {
            if (ctx->sw_bp[i].in_use && ctx->sw_bp[i].addr == addr) {
                uint32_t byte_addr = addr & GDB_AVR_ADDR_MASK;
                if (sw_bp_patch_flash(ctx->updi_fd, byte_addr,
                                      ctx->sw_bp[i].orig, NULL) < 0)
                    return reply_err(fd, "E01");
                ctx->sw_bp[i].in_use  = false;
                ctx->sw_bp[i].addr    = 0;
                ctx->sw_bp[i].orig[0] = 0;
                ctx->sw_bp[i].orig[1] = 0;
                return reply_ok(fd);
            }
        }
        /* Not in SW shadow — fall through to HW shadow scan below.   */
    }

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

/* HLR-061 / LLR-RSP-42: qThreadExtraInfo,<tid> — return a
 * human-readable label for the FSM identified by <tid>:
 *   "FSM <name> [active|quiescent] state=0x<state_fn>"
 * The label is hex-encoded byte-by-byte per the RSP spec.  Unknown
 * TIDs reply with the empty packet; "any-thread" forms (p0.0 / p-1.-1)
 * also reply empty — no aggregate label exists.                       */
static int dh_thread_extra(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    const char *p = strchr(pkt, ',');
    if (p == NULL) return reply_err(fd, "E01");
    ++p;
    uint32_t pid, tid;
    bool any;
    if (parse_mp_thread_id(&p, &pid, &tid, &any) < 0) return reply_err(fd, "E01");
    (void)pid;
    if (any) return reply_empty(fd);
    if (ctx->fsm == NULL || !ctx->fsm->valid) return reply_empty(fd);

    const FsmThread *match = NULL;
    for (int i = 0; i < ctx->fsm->thread_count; ++i) {
        if ((uint32_t)ctx->fsm->threads[i].gdb_id == tid) {
            match = &ctx->fsm->threads[i];
            break;
        }
    }
    if (match == NULL) return reply_empty(fd);

    /* Render label "FSM <name> [active|quiescent] state=0xNNNN".
     * Truncate the FSM name to 32 bytes so the assembled string fits
     * the 80-byte budget noted in LLR-RSP-42.                          */
    char name_buf[33];
    size_t nl = strnlen(match->name, sizeof name_buf - 1);
    memcpy(name_buf, match->name, nl);
    name_buf[nl] = '\0';

    char label[96];
    int lab_len = snprintf(label, sizeof label,
                           "FSM %s [%s] state=0x%04x",
                           name_buf,
                           match->is_active ? "active" : "quiescent",
                           (unsigned)match->state_fn);
    if (lab_len < 0) return reply_empty(fd);
    if ((size_t)lab_len >= sizeof label) lab_len = (int)sizeof label - 1;

    char reply[2 * sizeof label + 1];
    for (int i = 0; i < lab_len; ++i) byte_to_hex((uint8_t)label[i], &reply[i * 2]);
    reply[lab_len * 2] = '\0';
    return rsp_send_packet(fd, reply);
}

/* HLR-060 / LLR-RSP-41: parse the thread-id payload of an Hg/Hc/Hs
 * packet via parse_mp_thread_id().  *out_tid is set to the resolved
 * tid (the parsed value, or fsm_get_active_thread() when any=true).   */
static int parse_h_tid(const RspContext *ctx, const char *pkt, int *out_tid)
{
    /* Hg<tid> / Hc<tid> / Hs<tid> — skip the H<op> prefix. */
    const char *p = pkt + 2;
    uint32_t pid, tid;
    bool any;
    if (parse_mp_thread_id(&p, &pid, &tid, &any) < 0) return -1;
    (void)pid;
    if (any) {
        int active = (ctx != NULL && ctx->fsm) ? fsm_get_active_thread(ctx->fsm) : 1;
        if (active <= 0) active = 1;
        *out_tid = active;
    } else {
        *out_tid = (int)tid;
    }
    return 0;
}

static int dh_set_thread_g(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    int tid;
    if (parse_h_tid(ctx, pkt, &tid) < 0) return reply_err(fd, "E01");
    if (tid <= 0) tid = ctx->fsm ? fsm_get_active_thread(ctx->fsm) : 1;
    if (tid <= 0) tid = 1;
    if (ctx->g_thread_p) *ctx->g_thread_p = tid;
    return reply_ok(fd);
}

static int dh_set_thread_c(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    int tid;
    if (parse_h_tid(ctx, pkt, &tid) < 0) return reply_err(fd, "E01");
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
/* HLR-060 / LLR-RSP-41: T<tid> — is-thread-alive.  Accepts both legacy
 * bare-hex and multiprocess `p<PID>.<TID>` thread-id forms via
 * parse_mp_thread_id(); any-thread forms (p0.0 / p-1.-1 / -1) reply OK
 * without a membership check.                                         */
static int dh_thread_alive(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    const char *p = pkt + 1;            /* skip 'T' */
    uint32_t pid, tid;
    bool any;
    if (parse_mp_thread_id(&p, &pid, &tid, &any) < 0) return reply_err(fd, "E01");
    (void)pid;
    if (any) return reply_ok(fd);
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

/* Round v down to the nearest multiple of UPDI_FLASH_PAGE_SIZE. */
static uint32_t flash_page_floor(uint32_t v)
{
    return v - (v % UPDI_FLASH_PAGE_SIZE);
}

/* Round v up to the nearest multiple of UPDI_FLASH_PAGE_SIZE. */
static uint32_t flash_page_ceil(uint32_t v)
{
    uint32_t r = v % UPDI_FLASH_PAGE_SIZE;
    return (r == 0u) ? v : (v + UPDI_FLASH_PAGE_SIZE - r);
}

/* HLR-053 (LLR-RSP-26): vFlashErase:addr,length.  Validate that the
 * range falls entirely within the FLASH window (no data-space flag).
 * Allocate (or extend) a page-aligned 0xFF-filled buffer covering the
 * erased range and reply OK.  Erase-on-silicon is deferred to
 * updi_nvm_write_flash() in dh_vflash_done(), which page-erases each
 * page before programming.                                            */
static int dh_vflash_erase(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    const char *p = pkt + strlen("vFlashErase:");
    uint32_t addr = 0, len = 0;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E22");
    if (*p != ',')                    return reply_err(fd, "E22");
    ++p;
    if (parse_hex_u32(&p, &len) < 0)  return reply_err(fd, "E22");
    if (len == 0u)                    return reply_err(fd, "E22");

    /* Reject any range that strays into data space (EEPROM, USERROW,
     * FUSES, LOCK, SIGROW all have bit 23 set on the GDB side).       */
    if ((addr | (addr + len - 1u)) & GDB_AVR_DATA_FLAG) {
        return reply_err(fd, "E22");
    }
    /* Defensive upper bound — refuse > 1 MB to avoid runaway alloc.   */
    if (len > 0x100000u) return reply_err(fd, "E22");

    uint32_t pg_lo = flash_page_floor(addr);
    uint32_t pg_hi = flash_page_ceil(addr + len);
    size_t   need  = (size_t)(pg_hi - pg_lo);

    if (ctx->flash_xact_buf == NULL) {
        ctx->flash_xact_buf  = (uint8_t *)malloc(need);
        if (ctx->flash_xact_buf == NULL) return reply_err(fd, "E22");
        memset(ctx->flash_xact_buf, 0xFF, need);
        ctx->flash_xact_base = pg_lo;
        ctx->flash_xact_len  = need;
    } else {
        /* Extend the existing buffer to span the union of the old and
         * new ranges; newly-added pages are pre-filled with 0xFF.     */
        uint32_t old_lo  = ctx->flash_xact_base;
        uint32_t old_hi  = ctx->flash_xact_base +
                           (uint32_t)ctx->flash_xact_len;
        uint32_t new_lo  = (pg_lo < old_lo) ? pg_lo : old_lo;
        uint32_t new_hi  = (pg_hi > old_hi) ? pg_hi : old_hi;
        size_t   new_len = (size_t)(new_hi - new_lo);
        if (new_lo != old_lo || new_hi != old_hi) {
            uint8_t *nb = (uint8_t *)malloc(new_len);
            if (nb == NULL) return reply_err(fd, "E22");
            memset(nb, 0xFF, new_len);
            memcpy(nb + (old_lo - new_lo),
                   ctx->flash_xact_buf, ctx->flash_xact_len);
            free(ctx->flash_xact_buf);
            ctx->flash_xact_buf  = nb;
            ctx->flash_xact_base = new_lo;
            ctx->flash_xact_len  = new_len;
        }
    }
    return reply_ok(fd);
}

/* HLR-053 (LLR-RSP-27): vFlashWrite:addr:<binary>.  Decode RSP binary
 * escapes (0x7D XOR 0x20) and copy the payload into the page buffer
 * at the right offset.  Refuses if no transaction is active, the
 * address is outside the erased range, or the payload overflows it.  */
static int dh_vflash_write(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    if (ctx->flash_xact_buf == NULL) return reply_err(fd, "E22");

    const char *prefix = "vFlashWrite:";
    size_t plen = g_dispatch_packet_len;
    if (plen == 0u) plen = strlen(pkt);
    size_t pref_len = strlen(prefix);
    if (plen <= pref_len) { flash_xact_abort(ctx); return reply_err(fd, "E22"); }

    const char *p = pkt + pref_len;
    uint32_t addr = 0;
    if (parse_hex_u32(&p, &addr) < 0) { flash_xact_abort(ctx); return reply_err(fd, "E22"); }
    if (*p != ':')                    { flash_xact_abort(ctx); return reply_err(fd, "E22"); }
    ++p;

    if (addr & GDB_AVR_DATA_FLAG)        { flash_xact_abort(ctx); return reply_err(fd, "E22"); }
    if (addr < ctx->flash_xact_base)     { flash_xact_abort(ctx); return reply_err(fd, "E22"); }

    size_t off    = (size_t)(addr - ctx->flash_xact_base);
    size_t remain = plen - (size_t)(p - pkt);
    size_t out    = off;
    for (size_t i = 0; i < remain; ++i) {
        uint8_t b = (uint8_t)p[i];
        if (b == 0x7Du) {
            if (i + 1u >= remain)         { flash_xact_abort(ctx); return reply_err(fd, "E22"); }
            b = (uint8_t)(p[++i] ^ 0x20);
        }
        if (out >= ctx->flash_xact_len)   { flash_xact_abort(ctx); return reply_err(fd, "E22"); }
        ctx->flash_xact_buf[out++] = b;
    }
    return reply_ok(fd);
}

/* HLR-053 (LLR-RSP-28): vFlashDone.  Flush the accumulated page buffer
 * to FLASH via updi_nvm_write_flash() (which page-erases before each
 * write), call updi_enter_debug() to leave the CPU halted at reset,
 * clear the HW-breakpoint shadow because the underlying instructions
 * may have changed, and reply OK.                                     */
static int dh_vflash_done(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    if (ctx->flash_xact_buf == NULL) return reply_ok(fd);

    uint32_t updi_addr = UPDI_FLASH_BASE + ctx->flash_xact_base;
    int rc = updi_nvm_write_flash(ctx->updi_fd, updi_addr,
                                  ctx->flash_xact_buf,
                                  ctx->flash_xact_len);
    flash_xact_abort(ctx);
    if (rc < 0) return reply_err(fd, "E22");
    rsp_hw_bp_clear_all(ctx);
    rsp_sw_bp_clear_all(ctx);
    (void)updi_enter_debug(ctx->updi_fd);
    return reply_ok(fd);
}

/* HLR-058 (LLR-RSP-23): vRun;[<filename>][;<arg>...] — extended-remote
 * start-new-program.  avr-updi-gdb has no filesystem on the target and
 * the program is already in FLASH, so all arguments are ignored.  Per
 * the GDB protocol, vRun must immediately return a stop reply (the
 * client interprets it as "program loaded, halted at entry").  We
 * reset the AVR via updi_enter_debug() (halts at the reset vector),
 * invalidate and rebuild the FSM thread cache, and emit a T05 halt
 * reason.  The client is then free to issue its own `c` / `s` to
 * begin execution.                                                    */
static int dh_vrun(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;

    if (updi_enter_debug(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    fsm_invalidate(ctx->fsm);
    (void)fsm_build_thread_list(ctx->fsm, ctx->idx, ctx->updi_fd);
    return dh_halt_reason(fd, "?", vctx);
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
    /* HLR-054: drop any SW-BP shadow that survived memset(). */
    rsp_sw_bp_clear_all(ctx);

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
    h->on_vflash_erase = dh_vflash_erase;
    h->on_vflash_write = dh_vflash_write;
    h->on_vflash_done  = dh_vflash_done;
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
    return rsp_dispatch_n(fd, packet, (packet == NULL) ? 0u : strlen(packet), h);
}

int rsp_dispatch_n(int fd, const char *packet, size_t plen, RspHandlers *h)
{
    if (packet == NULL || plen == 0u || packet[0] == '\0') return reply_empty(fd);
    g_dispatch_packet_len = plen;

    /* ── Inline-handled (no target access) ─────────────────────────── */
    if (strncmp(packet, "qSupported", 10) == 0) {
        return rsp_send_packet(fd,
            "PacketSize=800;QStartNoAckMode+;multiprocess+;vContSupported+"
            ";vRun+;vAttach+;vKill+;vFlashErase+;vFlashWrite+;vFlashDone+");
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
    /* HLR-053: vFlash* — route before vCont/vRun checks. */
    if (strncmp(packet, "vFlashErase:", 12) == 0) {
        return call_handler(h->on_vflash_erase, fd, packet, h->ctx);
    }
    if (strncmp(packet, "vFlashWrite:", 12) == 0) {
        return call_handler(h->on_vflash_write, fd, packet, h->ctx);
    }
    if (strncmp(packet, "vFlashDone", 10) == 0) {
        return call_handler(h->on_vflash_done, fd, packet, h->ctx);
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

    /* HLR-053: any g/G/m/M/X/c/s that arrives mid-vFlash-transaction
     * is a protocol error; reject with E22 and discard the buffer.    */
    {
        RspContext *xctx = (RspContext *)h->ctx;
        char c0 = packet[0];
        bool target_access = (c0 == 'g' || c0 == 'G' || c0 == 'P' ||
                              c0 == 'm' || c0 == 'M' || c0 == 'X' ||
                              c0 == 'c' || c0 == 's');
        if (target_access && xctx != NULL && xctx->flash_xact_buf != NULL) {
            flash_xact_abort(xctx);
            return reply_err(fd, "E22");
        }
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
