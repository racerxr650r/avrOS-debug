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
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ── Static buffers ──────────────────────────────────────────────────── */

static char   pkt_buf[RSP_PACKET_MAX + 1];     /* not used by lib; reserved */
static char   rsp_buf[RSP_PACKET_MAX + 8];

typedef struct {
    uint32_t addr;
    uint16_t saved_word;
    bool     in_use;
} BpSlot;

static BpSlot bp_table[RSP_MAX_BREAKPOINTS];

static bool g_noack = false;

void rsp_set_noack(bool e) { g_noack = e; }
bool rsp_get_noack(void)   { return g_noack; }

/* Suppress unused-static warning for the reserved external receive buffer
 * (some callers will reuse pkt_buf via a future helper). */
__attribute__((used)) static char *_pkt_buf_keepalive = pkt_buf;

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
        csum ^= (uint8_t)c;
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
    for (size_t i = 0; i < plen; ++i) csum ^= (uint8_t)payload[i];
    rsp_buf[0] = '$';
    memcpy(rsp_buf + 1, payload, plen);
    rsp_buf[1 + plen] = '#';
    byte_to_hex(csum, &rsp_buf[2 + plen]);
    return (write_all(fd, rsp_buf, plen + 4u) < 0) ? -1 : 0;
}

/* ── Breakpoint table helpers ───────────────────────────────────────── */

static BpSlot *bp_find(uint32_t addr)
{
    for (size_t i = 0; i < RSP_MAX_BREAKPOINTS; ++i) {
        if (bp_table[i].in_use && bp_table[i].addr == addr) return &bp_table[i];
    }
    return NULL;
}

static BpSlot *bp_alloc(void)
{
    for (size_t i = 0; i < RSP_MAX_BREAKPOINTS; ++i) {
        if (!bp_table[i].in_use) return &bp_table[i];
    }
    return NULL;
}

static void bp_clear_all(void)
{
    memset(bp_table, 0, sizeof bp_table);
}

/* ── Default handlers ────────────────────────────────────────────────── */

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
    char reply[32];
    snprintf(reply, sizeof reply, "%sthread:%x;", RSP_STOP_SIGTRAP, (unsigned)aid);
    return rsp_send_packet(fd, reply);
}

static int dh_read_regs(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    char reg[80] = {0};
    int tid = ctx->g_thread_p ? *ctx->g_thread_p : 0;
    if (tid <= 0) tid = ctx->fsm ? fsm_get_active_thread(ctx->fsm) : 1;
    if (tid <= 0) tid = 1;
    if (fsm_get_registers(ctx->fsm, tid, reg) < 0) {
        return reply_err(fd, "E01");
    }
    return rsp_send_packet(fd, reg);
}

static int dh_write_regs(int fd, const char *pkt, void *vctx)
{
    /* Accepts both Gxx... and Pn=xx... */
    (void)vctx;
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
        RspContext *ctx = (RspContext *)vctx;
        if (updi_mem_write(ctx->updi_fd, 0x1000u + regnum, bytes, nb) < 0) {
            return reply_err(fd, "E01");
        }
        return reply_ok(fd);
    }
    /* G<78 hex chars> */
    const char *p = pkt + 1;
    size_t nibbles = strlen(p);
    if (nibbles < 64u || (nibbles & 1u)) return reply_err(fd, "E01");
    size_t nb = nibbles / 2u;
    uint8_t bytes[64];
    if (nb > sizeof bytes) nb = sizeof bytes;
    for (size_t i = 0; i < nb; ++i) {
        int hi = hex_nibble(p[i * 2]);
        int lo = hex_nibble(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) return reply_err(fd, "E01");
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    RspContext *ctx = (RspContext *)vctx;
    if (updi_mem_write(ctx->updi_fd, 0x1000u, bytes, nb) < 0) {
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

    uint8_t buf[512];
    if (updi_mem_read(ctx->updi_fd, addr, buf, len) < 0) {
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

    /* Heuristic: FLASH addresses are below SRAM (the host program-counter
     * space). avr8 SRAM begins at 0x800000 in GDB's unified addressing. */
    int rc;
    if (addr >= 0x800000u) {
        rc = updi_mem_write(ctx->updi_fd, addr & 0x7FFFFFu, data, len);
    } else {
        rc = updi_nvm_write_flash(ctx->updi_fd, addr, data, len);
    }
    return (rc < 0) ? reply_err(fd, "E01") : reply_ok(fd);
}

static int dh_continue(int fd, const char *pkt, void *vctx)
{
    (void)pkt;
    RspContext *ctx = (RspContext *)vctx;
    if (updi_run(ctx->updi_fd) < 0) return reply_err(fd, "E01");
    fsm_invalidate(ctx->fsm);
    /* Poll for halt by issuing updi_halt() until it succeeds. Returning to
     * the wire happens via a stop-reason packet. */
    while (updi_halt(ctx->updi_fd) < 0) {
        /* loop */
    }
    if (ctx->fsm) {
        (void)fsm_build_thread_list(ctx->fsm, ctx->idx, ctx->updi_fd);
    }
    return dh_halt_reason(fd, "?", vctx);
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

static int dh_insert_bp(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    /* Z0,addr,kind  — only Z0 (software bp) supported. */
    if (pkt[1] != '0') return reply_empty(fd);
    const char *p = pkt + 2;
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    uint32_t addr;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");

    if (bp_find(addr) != NULL) return reply_ok(fd);

    BpSlot *slot = bp_alloc();
    if (slot == NULL) return reply_err(fd, "E08");

    uint8_t orig[2];
    if (updi_mem_read(ctx->updi_fd, addr, orig, 2u) < 0) return reply_err(fd, "E01");
    uint16_t saved = (uint16_t)(orig[0] | ((uint16_t)orig[1] << 8));

    uint8_t brk[2] = { (uint8_t)(AVR_BREAK_OPCODE & 0xFFu),
                       (uint8_t)((AVR_BREAK_OPCODE >> 8) & 0xFFu) };
    if (updi_nvm_write_flash(ctx->updi_fd, addr, brk, 2u) < 0) {
        return reply_err(fd, "E01");
    }
    slot->addr       = addr;
    slot->saved_word = saved;
    slot->in_use     = true;
    return reply_ok(fd);
}

static int dh_remove_bp(int fd, const char *pkt, void *vctx)
{
    RspContext *ctx = (RspContext *)vctx;
    if (pkt[1] != '0') return reply_empty(fd);
    const char *p = pkt + 2;
    if (*p != ',') return reply_err(fd, "E01");
    ++p;
    uint32_t addr;
    if (parse_hex_u32(&p, &addr) < 0) return reply_err(fd, "E01");

    BpSlot *slot = bp_find(addr);
    if (slot == NULL) return reply_err(fd, "E02");

    uint8_t orig[2] = { (uint8_t)(slot->saved_word & 0xFFu),
                        (uint8_t)((slot->saved_word >> 8) & 0xFFu) };
    if (updi_nvm_write_flash(ctx->updi_fd, addr, orig, 2u) < 0) {
        return reply_err(fd, "E01");
    }
    slot->in_use = false;
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
    int rc = monitor_dispatch(fd, ctx->updi_fd, ctx->idx, p);
    if (rc == 0) return reply_ok(fd);
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
    /* D */
    (void)updi_run(ctx->updi_fd);
    (void)reply_ok(fd);
    if (ctx->gdb_fd_p) {
        if (*ctx->gdb_fd_p >= 0) rsp_close(*ctx->gdb_fd_p);
        *ctx->gdb_fd_p = -1;
    }
    bp_clear_all();
    return 0;
}

void rsp_default_handlers(RspHandlers *h, RspContext *ctx)
{
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
            "PacketSize=800;QStartNoAckMode+;multiprocess-;vContSupported+");
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
            return reply_empty(fd);
        default:
            return reply_empty(fd);
    }
}
