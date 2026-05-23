/* src/monitor.c — `monitor avros` sub-command handler.
 *
 * Implements the `qRcmd` GDB monitor dispatcher for the avrOS-specific
 * introspection commands. All target reads use updi_mem_read() (background,
 * non-intrusive); no helper here ever halts the CPU.
 *
 * Output protocol: each line of human-readable text is hex-encoded and
 * delivered as an RSP O-packet by rsp_send_packet().
 */
#include "monitor.h"
#include "gdb_rsp.h"
#include "updi.h"
#include "fsm_mapper.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* avrOS runtime descriptor sizes (must match sys/fsm.c, sys/queue.c, sys/event.c) */
#define EVNT_DESCR_SIZE     4u
#define QUE_DESCR_SIZE      10u

#define MON_TEXT_BUF_SIZE   512u
#define MON_NAME_MAX        32u
#define MON_O_PACKET_MAX    1100u   /* "O" + 2*512 + NUL slack */

/* ── helpers ──────────────────────────────────────────────────────────── */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode `in` (ASCII hex pairs) into `out`. Returns decoded byte count, or
 * -1 if length is odd or any character is not a hex digit. `out` is NUL-
 * terminated on success. */
static int hex_decode(const char *in, char *out, size_t out_max)
{
    size_t len = strlen(in);
    if ((len & 1u) != 0u) return -1;
    size_t n = len / 2u;
    if (n + 1u > out_max) return -1;
    for (size_t i = 0; i < n; ++i) {
        int hi = hex_nibble(in[i * 2u]);
        int lo = hex_nibble(in[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (char)((hi << 4) | lo);
    }
    out[n] = '\0';
    return (int)n;
}

/* Hex-encode `text` (length `text_len`) into `out` as an RSP O-packet body:
 * one leading 'O' followed by 2 * text_len uppercase hex digits and a NUL.
 * Returns the encoded payload length (excluding NUL), or -1 if `out_max`
 * is too small. */
static int build_o_packet(const char *text, size_t text_len,
                          char *out, size_t out_max)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t need = 1u + (text_len * 2u) + 1u;
    if (need > out_max) return -1;
    out[0] = 'O';
    for (size_t i = 0; i < text_len; ++i) {
        uint8_t b = (uint8_t)text[i];
        out[1u + i * 2u]      = hex[(b >> 4) & 0xFu];
        out[1u + i * 2u + 1u] = hex[b & 0xFu];
    }
    out[1u + text_len * 2u] = '\0';
    return (int)(1u + text_len * 2u);
}

/* Hex-encode `text` and ship it as an RSP O-packet on `rsp_fd`.
 * Returns 0 on success, -1 on send / encoding failure. */
static int send_text_as_o_packet(int rsp_fd, const char *text, size_t text_len)
{
    char pkt[MON_O_PACKET_MAX];
    if (build_o_packet(text, text_len, pkt, sizeof pkt) < 0) return -1;
    return rsp_send_packet(rsp_fd, pkt);
}

/* Read a NUL-terminated string from the target starting at GDB-AVR
 * byte address `addr` via background UPDI reads.  String literals on
 * AVR-Dx live in the mapped-flash window (>= 0x8000); we route through
 * UPDI's flash mirror so the chip resolves FLMAP transparently.
 * Returns the number of payload bytes copied into `out` (excluding
 * NUL), or -1 on UPDI failure.  Always NUL-terminates when len >= 1. */
static int read_target_string(int updi_fd, uint32_t addr,
                              char *out, size_t out_max)
{
    if (out_max == 0) return -1;
    uint32_t base = flash_to_updi(addr);
    size_t n = 0;
    while (n + 1u < out_max) {
        uint8_t b;
        if (updi_mem_read(updi_fd, base + (uint32_t)n, &b, 1u) < 0) return -1;
        if (b == 0u) break;
        if (!isprint((unsigned char)b)) b = '?';
        out[n++] = (char)b;
    }
    out[n] = '\0';
    return (int)n;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ── sub-command: events ─────────────────────────────────────────────── */

static int cmd_events(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx)
{
    if (idx->event_count == 0u) {
        const char *msg = "  (no events registered)\n";
        return send_text_as_o_packet(rsp_fd, msg, strlen(msg));
    }

    static uint8_t descr[EVNT_DESCR_SIZE * 256u];
    size_t total = (size_t)idx->event_count * EVNT_DESCR_SIZE;
    if (updi_mem_read(updi_fd, flash_to_updi(idx->event_table_addr),
                      descr, total) < 0) {
        return -1;
    }

    char text[MON_TEXT_BUF_SIZE];
    size_t off = 0;

    for (uint8_t i = 0; i < idx->event_count; ++i) {
        const uint8_t *d = &descr[i * EVNT_DESCR_SIZE];
        uint16_t name_ptr   = le16(&d[0]);
        uint16_t status_ptr = le16(&d[2]);

        char name[MON_NAME_MAX];
        if (name_ptr == 0u) {
            snprintf(name, sizeof name, "<null>");
        } else if (read_target_string(updi_fd, name_ptr, name, sizeof name) < 0) {
            return -1;
        }

        uint8_t status = 0u;
        if (status_ptr != 0u &&
            updi_mem_read(updi_fd, status_ptr, &status, 1u) < 0) {
            return -1;
        }

        int n = snprintf(text + off, sizeof text - off,
                         "  %s: 0x%02X\n", name, status);
        if (n < 0) return -1;
        if ((size_t)n >= sizeof text - off) {
            off = sizeof text - 1u;
            break;
        }
        off += (size_t)n;
    }

    return send_text_as_o_packet(rsp_fd, text, off);
}

/* ── sub-command: queues ─────────────────────────────────────────────── */

static int cmd_queues(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx)
{
    if (idx->queue_count == 0u) {
        const char *msg = "  (no queues registered)\n";
        return send_text_as_o_packet(rsp_fd, msg, strlen(msg));
    }

    static uint8_t descr[QUE_DESCR_SIZE * 256u];
    size_t total = (size_t)idx->queue_count * QUE_DESCR_SIZE;
    if (updi_mem_read(updi_fd, flash_to_updi(idx->queue_table_addr),
                      descr, total) < 0) {
        return -1;
    }

    char text[MON_TEXT_BUF_SIZE];
    size_t off = 0;

    for (uint8_t i = 0; i < idx->queue_count; ++i) {
        const uint8_t *d = &descr[i * QUE_DESCR_SIZE];
        /* layout: queue*(2) buffer*(2) event*(2) capacity(2) sizeOfElement(2) */
        uint16_t capacity = le16(&d[6]);
        uint16_t elem_sz  = le16(&d[8]);

        int n = snprintf(text + off, sizeof text - off,
                         "  queue[%u]: capacity=%u sizeOfElement=%u\n",
                         (unsigned)i, (unsigned)capacity, (unsigned)elem_sz);
        if (n < 0) return -1;
        if ((size_t)n >= sizeof text - off) {
            off = sizeof text - 1u;
            break;
        }
        off += (size_t)n;
    }

    return send_text_as_o_packet(rsp_fd, text, off);
}

/* ── usage hint ──────────────────────────────────────────────────────── */

static void send_usage_hint(int rsp_fd)
{
    static const char usage[] =
        "usage: monitor avros <events|queues>\n";
    (void)send_text_as_o_packet(rsp_fd, usage, sizeof usage - 1u);
}

/* ── public entry point ──────────────────────────────────────────────── */

int monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx,
                     const char *cmd)
{
    if (idx == NULL || cmd == NULL) return -2;

    char decoded[256];
    int dlen = hex_decode(cmd, decoded, sizeof decoded);
    if (dlen < 0) {
        return -2;
    }

    static const char prefix[] = "avros ";
    const size_t plen = sizeof prefix - 1u;
    if ((size_t)dlen < plen || strncmp(decoded, prefix, plen) != 0) {
        send_usage_hint(rsp_fd);
        return -2;
    }

    const char *sub = decoded + plen;
    if (strcmp(sub, "events") == 0) {
        return cmd_events(rsp_fd, updi_fd, idx);
    }
    if (strcmp(sub, "queues") == 0) {
        return cmd_queues(rsp_fd, updi_fd, idx);
    }

    send_usage_hint(rsp_fd);
    return -2;
}

/* ──────────────────────────────────────────────────────────────────────
 * HLR-055: avarice-compatible top-level monitor verbs.
 * Implemented here so the dispatcher is one consistent module.  These
 * verbs take an RspContext to reach the FSM cache, BP shadow, and the
 * `--allow-erase` gate.
 * ──────────────────────────────────────────────────────────────────── */

#ifndef AVROSDB_VERSION
#define AVROSDB_VERSION "unknown"
#endif

/* Emit a single hex-encoded O-packet line.  Returns 0/-1. */
static int o_line(int rsp_fd, const char *line)
{
    return send_text_as_o_packet(rsp_fd, line, strlen(line));
}

static int verb_reset(int rsp_fd, RspContext *ctx)
{
    /* updi_enter_debug() applies an ASI_RESET pulse and re-enters OCD
     * so the CPU lands halted at the reset vector — exactly the
     * semantic mandated by HLR-055.                                  */
    if (updi_enter_debug(ctx->updi_fd) < 0) return -1;
    rsp_hw_bp_clear_all(ctx);
    rsp_sw_bp_clear_all(ctx);   /* HLR-054 */
    if (ctx->fsm != NULL) fsm_invalidate(ctx->fsm);
    (void)o_line(rsp_fd, "target reset, halted at reset vector\n");
    return 0;
}

static int verb_halt(int rsp_fd, RspContext *ctx)
{
    if (updi_halt(ctx->updi_fd) < 0) return -1;
    /* Stay within qRcmd protocol: emit a human-readable O-packet line
     * and let dh_monitor send the default `OK`. GDB will re-fetch
     * registers on the next `info`/`print` command via $g, so PC will
     * reflect the post-halt state. (T05 is *not* a valid reply to
     * qRcmd — sending it here triggers "Invalid hex digit" parse
     * errors in the gdb client.) */
    (void)o_line(rsp_fd, "target halted\n");
    return 0;
}

static int verb_go(int rsp_fd, RspContext *ctx)
{
    if (updi_run(ctx->updi_fd) < 0) return -1;
    (void)o_line(rsp_fd, "target running\n");
    return 0;
}

static int verb_erase(int rsp_fd, RspContext *ctx)
{
    if (!ctx->allow_erase) {
        (void)o_line(rsp_fd,
            "monitor erase: refused — launch the server with "
            "--allow-erase to enable.\n");
        return 0;
    }
    if (updi_chip_erase(ctx->updi_fd) < 0) return -1;
    /* The chip is now blank and in NVMPROG-exit state.  Re-enter OCD
     * so subsequent register/PC reads succeed, and drop all BP shadow
     * entries because FLASH no longer holds any patched instructions. */
    if (updi_enter_debug(ctx->updi_fd) < 0) return -1;
    rsp_hw_bp_clear_all(ctx);
    rsp_sw_bp_clear_all(ctx);   /* HLR-054 */
    if (ctx->fsm != NULL) fsm_invalidate(ctx->fsm);
    (void)o_line(rsp_fd, "chip-erase complete; target halted\n");
    return 0;
}

static int verb_version(int rsp_fd)
{
    char line[128];
    snprintf(line, sizeof line,
             "avrOSdb %s (built " __DATE__ ")\n", AVROSDB_VERSION);
    (void)o_line(rsp_fd, line);
    return 0;
}

static int verb_bp_mode(int rsp_fd, RspContext *ctx, const char *arg)
{
    if (arg == NULL || arg[0] == '\0') {
        char line[64];
        snprintf(line, sizeof line, "bp-mode: %s\n",
                 ctx->bp_mode == RSP_BP_MODE_HW_ONLY ? "hw-only" : "sw");
        (void)o_line(rsp_fd, line);
        return 0;
    }
    if (strcmp(arg, "sw") == 0) {
        ctx->bp_mode = RSP_BP_MODE_SW;
        (void)o_line(rsp_fd, "bp-mode: sw\n");
        return 0;
    }
    if (strcmp(arg, "hw-only") == 0) {
        ctx->bp_mode = RSP_BP_MODE_HW_ONLY;
        (void)o_line(rsp_fd, "bp-mode: hw-only\n");
        return 0;
    }
    (void)o_line(rsp_fd, "bp-mode: argument must be 'sw' or 'hw-only'\n");
    return -2;
}

static int verb_help(int rsp_fd)
{
    static const char *const lines[] = {
        "monitor reset            — reset target, halt at reset vector\n",
        "monitor halt             — halt target now (emits T05 stop-reply)\n",
        "monitor go               — resume target (OK; stop-reply on next halt)\n",
        "monitor erase            — chip-erase (requires --allow-erase)\n",
        "monitor chip-erase       — alias of `monitor erase`\n",
        "monitor version          — print server version + build date\n",
        "monitor bp-mode <sw|hw-only> — select breakpoint installation policy\n",
        "monitor help             — this help text\n",
        "monitor avros events     — list avrOS event descriptors\n",
        "monitor avros queues     — list avrOS queue descriptors\n",
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) {
        if (o_line(rsp_fd, lines[i]) < 0) return -1;
    }
    return 0;
}

/* Trim leading whitespace from `s`. */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

int monitor_dispatch_ex(int rsp_fd, RspContext *ctx, const char *cmd)
{
    if (ctx == NULL || cmd == NULL) return -2;

    /* Decode the hex blob into the same buffer monitor_dispatch uses
     * so the avros sub-dispatch keeps working.  We need a writable
     * decoded copy because we tokenise it for top-level verbs.       */
    char decoded[256];
    int dlen = hex_decode(cmd, decoded, sizeof decoded);
    if (dlen < 0) return -2;

    const char *p = skip_ws(decoded);

    /* `avros ...` delegates to the legacy dispatcher (which itself
     * re-decodes — accept that small cost rather than refactoring).  */
    static const char avros[] = "avros ";
    if (strncmp(p, avros, sizeof avros - 1u) == 0) {
        return monitor_dispatch(rsp_fd, ctx->updi_fd, ctx->idx, cmd);
    }

    if (strcmp(p, "reset") == 0)       return verb_reset(rsp_fd, ctx);
    if (strcmp(p, "halt")  == 0)       return verb_halt(rsp_fd, ctx);
    if (strcmp(p, "go")    == 0)       return verb_go(rsp_fd, ctx);
    if (strcmp(p, "erase") == 0 ||
        strcmp(p, "chip-erase") == 0)  return verb_erase(rsp_fd, ctx);
    if (strcmp(p, "version") == 0)     return verb_version(rsp_fd);
    if (strcmp(p, "help")    == 0)     return verb_help(rsp_fd);

    static const char bpm[] = "bp-mode";
    if (strncmp(p, bpm, sizeof bpm - 1u) == 0) {
        const char *arg = skip_ws(p + sizeof bpm - 1u);
        return verb_bp_mode(rsp_fd, ctx, arg);
    }

    (void)o_line(rsp_fd,
        "usage: monitor <reset|halt|go|erase|chip-erase|version|"
        "bp-mode|help|avros …>\n");
    return -2;
}
