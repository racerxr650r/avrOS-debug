/* src/dap.c — Debug Adapter Protocol (DAP) front-end.
 *
 * Phase 16: this file provides the DAP **wire transport** — Content-Length
 * message framing plus a small self-contained JSON codec — which the
 * lifecycle/request handlers build on. Both halves are independent of the
 * target, so they are unit-tested (tests/test_dap.c) over a pipe without
 * hardware. `dap_serve()` (the accept + dispatch loop and the
 * initialize/launch/configurationDone/disconnect handlers) is still a
 * scaffold; it lands next on this branch. See doc/SDP.md §8 Phase 16 and
 * doc/reference/dual-protocol-architecture.md.
 */
#include "dap.h"
#include "gdb_rsp.h"   /* rsp_accept / rsp_close — shared TCP transport helpers */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/* ── JSON tokenizer (recursive descent → flat pre-order token array) ──────── */

typedef struct {
    const char *js;
    size_t      len;
    size_t      pos;
    dj_tok_t   *t;
    int         max;
    int         n;
} dj_parser;

static void dj_skip_ws(dj_parser *p)
{
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static int dj_alloc(dj_parser *p)
{
    if (p->n >= p->max)
        return -1;
    return p->n++;
}

static int dj_value(dj_parser *p);   /* forward */

static int dj_string_tok(dj_parser *p)
{
    int ti = dj_alloc(p);
    if (ti < 0)
        return -1;
    p->pos++;                         /* opening quote */
    int s = (int)p->pos;
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == '\\') {              /* skip the escaped char (incl. \uXXXX
                                       * tail, which scans as ordinary chars) */
            p->pos += 2;
            continue;
        }
        if (c == '"') {
            p->t[ti].type  = DJ_STRING;
            p->t[ti].start = s;
            p->t[ti].end   = (int)p->pos;
            p->t[ti].size  = 0;
            p->pos++;                 /* closing quote */
            p->t[ti].next  = p->n;
            return 0;
        }
        p->pos++;
    }
    return -1;                        /* unterminated */
}

static int dj_prim_tok(dj_parser *p)
{
    int ti = dj_alloc(p);
    if (ti < 0)
        return -1;
    int       s = (int)p->pos;
    dj_type_t ty;
    char      c = p->js[p->pos];

    if (c == 't') {
        if (p->pos + 4 > p->len || memcmp(p->js + p->pos, "true", 4) != 0)
            return -1;
        p->pos += 4; ty = DJ_TRUE;
    } else if (c == 'f') {
        if (p->pos + 5 > p->len || memcmp(p->js + p->pos, "false", 5) != 0)
            return -1;
        p->pos += 5; ty = DJ_FALSE;
    } else if (c == 'n') {
        if (p->pos + 4 > p->len || memcmp(p->js + p->pos, "null", 4) != 0)
            return -1;
        p->pos += 4; ty = DJ_NULL;
    } else {                          /* number */
        ty = DJ_NUMBER;
        while (p->pos < p->len) {
            char d = p->js[p->pos];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' ||
                d == '.' || d == 'e' || d == 'E')
                p->pos++;
            else
                break;
        }
        if ((int)p->pos == s)
            return -1;
    }
    p->t[ti].type  = ty;
    p->t[ti].start = s;
    p->t[ti].end   = (int)p->pos;
    p->t[ti].size  = 0;
    p->t[ti].next  = p->n;
    return 0;
}

static int dj_array_tok(dj_parser *p)
{
    int ti = dj_alloc(p);
    if (ti < 0)
        return -1;
    p->t[ti].type  = DJ_ARRAY;
    p->t[ti].start = (int)p->pos;
    p->t[ti].size  = 0;
    p->pos++;                         /* '[' */
    dj_skip_ws(p);
    if (p->pos < p->len && p->js[p->pos] == ']') {
        p->pos++;
        p->t[ti].end  = (int)p->pos;
        p->t[ti].next = p->n;
        return 0;
    }
    for (;;) {
        dj_skip_ws(p);
        if (dj_value(p) < 0)
            return -1;
        p->t[ti].size++;
        dj_skip_ws(p);
        if (p->pos >= p->len)
            return -1;
        char c = p->js[p->pos++];
        if (c == ',')
            continue;
        if (c == ']') {
            p->t[ti].end  = (int)p->pos;
            p->t[ti].next = p->n;
            return 0;
        }
        return -1;
    }
}

static int dj_object_tok(dj_parser *p)
{
    int ti = dj_alloc(p);
    if (ti < 0)
        return -1;
    p->t[ti].type  = DJ_OBJECT;
    p->t[ti].start = (int)p->pos;
    p->t[ti].size  = 0;
    p->pos++;                         /* '{' */
    dj_skip_ws(p);
    if (p->pos < p->len && p->js[p->pos] == '}') {
        p->pos++;
        p->t[ti].end  = (int)p->pos;
        p->t[ti].next = p->n;
        return 0;
    }
    for (;;) {
        dj_skip_ws(p);
        if (p->pos >= p->len || p->js[p->pos] != '"')
            return -1;
        if (dj_string_tok(p) < 0)     /* key */
            return -1;
        dj_skip_ws(p);
        if (p->pos >= p->len || p->js[p->pos] != ':')
            return -1;
        p->pos++;
        dj_skip_ws(p);
        if (dj_value(p) < 0)          /* value */
            return -1;
        p->t[ti].size++;
        dj_skip_ws(p);
        if (p->pos >= p->len)
            return -1;
        char c = p->js[p->pos++];
        if (c == ',')
            continue;
        if (c == '}') {
            p->t[ti].end  = (int)p->pos;
            p->t[ti].next = p->n;
            return 0;
        }
        return -1;
    }
}

static int dj_value(dj_parser *p)
{
    dj_skip_ws(p);
    if (p->pos >= p->len)
        return -1;
    char c = p->js[p->pos];
    if (c == '{') return dj_object_tok(p);
    if (c == '[') return dj_array_tok(p);
    if (c == '"') return dj_string_tok(p);
    return dj_prim_tok(p);
}

int dj_parse(const char *js, size_t len, dj_tok_t *toks, int max)
{
    if (js == NULL || toks == NULL || max <= 0 || len > (size_t)INT_MAX)
        return -1;
    dj_parser p = { js, len, 0, toks, max, 0 };
    if (dj_value(&p) < 0)
        return -1;
    return p.n;
}

/* ── JSON accessors ───────────────────────────────────────────────────────── */

int dj_streq(const char *js, const dj_tok_t *t, int idx, const char *s)
{
    if (t[idx].type != DJ_STRING)
        return 0;
    size_t n = (size_t)(t[idx].end - t[idx].start);
    return strncmp(js + t[idx].start, s, n) == 0 && s[n] == '\0';
}

int dj_member(const char *js, const dj_tok_t *t, int obj, const char *key)
{
    if (t[obj].type != DJ_OBJECT)
        return -1;
    int cur = obj + 1;
    for (int i = 0; i < t[obj].size; i++) {
        int k = cur;
        int v = t[k].next;
        if (dj_streq(js, t, k, key))
            return v;
        cur = t[v].next;
    }
    return -1;
}

size_t dj_strcpy(const char *js, const dj_tok_t *t, int idx,
                 char *buf, size_t cap)
{
    if (cap == 0)
        return 0;
    size_t o = 0;
    int    i = t[idx].start, e = t[idx].end;
    while (i < e && o + 1 < cap) {
        char c = js[i++];
        if (c == '\\' && i < e) {
            char x = js[i++];
            switch (x) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '/': c = '/';  break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case 'u': {
                    unsigned v = 0;
                    if (i + 4 <= e) {
                        for (int k = 0; k < 4; k++) {
                            char h = js[i + k];
                            v <<= 4;
                            if      (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                        }
                        i += 4;
                        c = (v < 0x80u) ? (char)v : '?';
                    } else {
                        c = '?';
                    }
                    break;
                }
                default: c = x; break;
            }
        }
        buf[o++] = c;
    }
    buf[o] = '\0';
    return o;
}

int dj_long(const char *js, const dj_tok_t *t, int idx, long *out)
{
    if (t[idx].type != DJ_NUMBER)
        return -1;
    char   tmp[32];
    size_t n = (size_t)(t[idx].end - t[idx].start);
    if (n >= sizeof tmp)
        return -1;
    memcpy(tmp, js + t[idx].start, n);
    tmp[n] = '\0';
    char *end;
    long  v = strtol(tmp, &end, 10);
    if (end == tmp)
        return -1;
    *out = v;
    return 0;
}

size_t dj_escape(const char *s, char *buf, size_t cap)
{
    if (cap == 0)
        return 0;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        const char *seg;
        size_t      seglen;
        char        two[3] = { '\\', 0, 0 };
        switch (*p) {
            case '"':  two[1] = '"';  seg = two; seglen = 2; break;
            case '\\': two[1] = '\\'; seg = two; seglen = 2; break;
            case '\n': two[1] = 'n';  seg = two; seglen = 2; break;
            case '\r': two[1] = 'r';  seg = two; seglen = 2; break;
            case '\t': two[1] = 't';  seg = two; seglen = 2; break;
            case '\b': two[1] = 'b';  seg = two; seglen = 2; break;
            case '\f': two[1] = 'f';  seg = two; seglen = 2; break;
            default:
                if (*p < 0x20) {
                    int hl = snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*p);
                    seg = esc; seglen = (hl > 0) ? (size_t)hl : 0;
                } else {
                    two[0] = (char)*p; seg = two; seglen = 1;
                }
                break;
        }
        if (o + seglen + 1 > cap)
            break;
        memcpy(buf + o, seg, seglen);
        o += seglen;
    }
    buf[o] = '\0';
    return o;
}

/* ── Message framing ──────────────────────────────────────────────────────── */

static int read_full(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r == 0)  return 0;
        if (r < 0)   { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 1;
}

static int write_full(int fd, const char *buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, buf + put, n - put);
        if (r < 0)   { if (errno == EINTR) continue; return -1; }
        put += (size_t)r;
    }
    return 0;
}

/* Read one CRLF-terminated header line (the trailing CR/LF are stripped).
 * Returns 1 on a line, 0 on immediate EOF, -1 on error. */
static int read_header_line(int fd, char *buf, size_t cap, size_t *len)
{
    size_t o = 0;
    int    any = 0;
    for (;;) {
        char    c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) { if (len) *len = o; buf[o < cap ? o : cap - 1] = '\0';
                      return any ? 1 : 0; }
        if (r < 0)  { if (errno == EINTR) continue; return -1; }
        any = 1;
        if (c == '\n') { buf[o < cap ? o : cap - 1] = '\0'; if (len) *len = o; return 1; }
        if (c != '\r' && o + 1 < cap) buf[o++] = c;
    }
}

int dap_read_message(int fd, char *buf, size_t cap, size_t *out_len)
{
    long clen = -1;
    char line[256];
    for (;;) {
        size_t ll;
        int    r = read_header_line(fd, line, sizeof line, &ll);
        if (r <= 0)  return r;          /* 0 EOF, -1 error */
        if (ll == 0) break;             /* blank line ends the header block */
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            clen = strtol(line + 15, NULL, 10);
    }
    if (clen < 0 || (size_t)clen >= cap)
        return -1;
    int r = read_full(fd, buf, (size_t)clen);
    if (r <= 0)
        return r;
    buf[clen] = '\0';
    if (out_len)
        *out_len = (size_t)clen;
    return 1;
}

int dap_write_message(int fd, const char *body, size_t len)
{
    char hdr[64];
    int  hl = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", len);
    if (hl < 0)
        return -1;
    if (write_full(fd, hdr, (size_t)hl) < 0)
        return -1;
    if (write_full(fd, body, len) < 0)
        return -1;
    return 0;
}

/* ── Response / event builders ────────────────────────────────────────────── */

#define DAP_OUT_MAX 2048u   /* outgoing handshake messages are small */

static long dap_next_seq(dap_session *s) { return ++s->out_seq; }

static int dap_send_response(dap_session *s, long req_seq, const char *cmd,
                             bool success, const char *body_json)
{
    char cmde[64];
    dj_escape(cmd, cmde, sizeof cmde);
    char buf[DAP_OUT_MAX];
    int  n;
    if (body_json != NULL && body_json[0] != '\0')
        n = snprintf(buf, sizeof buf,
                     "{\"seq\":%ld,\"type\":\"response\",\"request_seq\":%ld,"
                     "\"success\":%s,\"command\":\"%s\",\"body\":%s}",
                     dap_next_seq(s), req_seq, success ? "true" : "false",
                     cmde, body_json);
    else
        n = snprintf(buf, sizeof buf,
                     "{\"seq\":%ld,\"type\":\"response\",\"request_seq\":%ld,"
                     "\"success\":%s,\"command\":\"%s\"}",
                     dap_next_seq(s), req_seq, success ? "true" : "false", cmde);
    if (n < 0 || (size_t)n >= sizeof buf)
        return -1;
    return dap_write_message(s->fd, buf, (size_t)n);
}

static int dap_send_error(dap_session *s, long req_seq, const char *cmd,
                          const char *message)
{
    char cmde[64], msge[256];
    dj_escape(cmd, cmde, sizeof cmde);
    dj_escape(message, msge, sizeof msge);
    char buf[DAP_OUT_MAX];
    int  n = snprintf(buf, sizeof buf,
                      "{\"seq\":%ld,\"type\":\"response\",\"request_seq\":%ld,"
                      "\"success\":false,\"command\":\"%s\",\"message\":\"%s\"}",
                      dap_next_seq(s), req_seq, cmde, msge);
    if (n < 0 || (size_t)n >= sizeof buf)
        return -1;
    return dap_write_message(s->fd, buf, (size_t)n);
}

static int dap_send_event(dap_session *s, const char *event,
                          const char *body_json)
{
    char eve[64];
    dj_escape(event, eve, sizeof eve);
    char buf[DAP_OUT_MAX];
    int  n;
    if (body_json != NULL && body_json[0] != '\0')
        n = snprintf(buf, sizeof buf,
                     "{\"seq\":%ld,\"type\":\"event\",\"event\":\"%s\","
                     "\"body\":%s}", dap_next_seq(s), eve, body_json);
    else
        n = snprintf(buf, sizeof buf,
                     "{\"seq\":%ld,\"type\":\"event\",\"event\":\"%s\"}",
                     dap_next_seq(s), eve);
    if (n < 0 || (size_t)n >= sizeof buf)
        return -1;
    return dap_write_message(s->fd, buf, (size_t)n);
}

/* Emit a `stopped` event for the single CPU thread.  `reason` is one of the
 * DAP-defined run-state reasons ("entry", "step", "breakpoint", "pause") and
 * is a fixed internal literal, so no escaping is needed.  (Phase 17) */
static int dap_emit_stopped(dap_session *s, const char *reason)
{
    char body[96];
    (void)snprintf(body, sizeof body,
                   "{\"reason\":\"%s\",\"threadId\":1,"
                   "\"allThreadsStopped\":true}", reason);
    return dap_send_event(s, "stopped", body);
}

/* Format one DAP stackFrame object into `out`.  Resolves `pc` to file:line via
 * libdw when a DWARF-bearing ELF is attached; otherwise emits a frame with no
 * source (line 0) so the client still has a valid entry. */
static void dap_format_frame(dap_session *s, int id, uint32_t pc,
                             char *out, size_t cap)
{
    char file[256];
    int  line = 0;
    if (s->elf != NULL &&
        elf_addr_to_line(s->elf, pc, file, sizeof file, &line) == 0) {
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        char pesc[600], besc[200];
        dj_escape(file, pesc, sizeof pesc);
        dj_escape(base, besc, sizeof besc);
        (void)snprintf(out, cap,
            "{\"id\":%d,\"name\":\"0x%06lx\",\"line\":%d,\"column\":1,"
            "\"instructionPointerReference\":\"0x%lx\","
            "\"source\":{\"name\":\"%s\",\"path\":\"%s\"}}",
            id, (unsigned long)pc, line, (unsigned long)pc, besc, pesc);
    } else {
        (void)snprintf(out, cap,
            "{\"id\":%d,\"name\":\"0x%06lx\",\"line\":0,\"column\":1,"
            "\"instructionPointerReference\":\"0x%lx\"}",
            id, (unsigned long)pc, (unsigned long)pc);
    }
}

#define DAP_MAX_FRAMES 32

/* Phase 18: multi-frame stack unwind via .debug_frame CFI + the live target.
 *
 * For each frame we get the CFA rule (CFA = value(cfa_reg) + offset) from
 * elf_cfi_cfa(), where cfa_reg is the Y frame-pointer pair r28:r29 (28) or SP
 * (32).  AVR's fixed stack conventions then give the caller:
 *   - the 2-byte word return address sits just below the CFA (a `call` pushes
 *     the word PC); the code-space byte PC is that word << 1;
 *   - the caller's Y was saved by the callee prologue (push r28; push r29), so
 *     r28 (Y-low) is the byte just under the return address and r29 (Y-high)
 *     one lower still.
 * The caller's SP at its call site is the CFA.  We stop at an invalid return
 * address (0, odd-of-range), when the CFA stops advancing, or at DAP_MAX_FRAMES.
 * The exact byte offsets are AVR-silicon conventions, locked against avr-gdb's
 * backtrace on the gdb_debug_session fixture (DAP9, doc/UserManual Appendix B).
 *
 * Fills pcs[0..n-1] (frame 0 = innermost) and returns n (>= 1), or 0 if the
 * live registers can't be read. */
static int dap_unwind(dap_session *s, uint32_t *pcs, int max)
{
    uint32_t pc = 0, y;
    uint16_t sp16 = 0;
    uint8_t  r28 = 0, r29 = 0;

    if (s->updi_fd < 0 || s->elf == NULL) return 0;
    if (updi_ocd_read_pc (s->updi_fd, &pc)      != 0) return 0;
    if (updi_ocd_read_sp (s->updi_fd, &sp16)    != 0) return 0;
    if (updi_ocd_read_gpr(s->updi_fd, 28, &r28) != 0) return 0;
    if (updi_ocd_read_gpr(s->updi_fd, 29, &r29) != 0) return 0;

    uint32_t sp = sp16;
    y = (uint32_t)r28 | ((uint32_t)r29 << 8);

    uint32_t flash_size = s->elf->flash_size;
    const bool dbg = getenv("AVROSDB_DAP_UNWIND_LOG") != NULL;

    int n = 0;
    pcs[n++] = pc;
    uint32_t prev_cfa = 0;
    while (n < max) {
        int cfa_reg = 0, cfa_off = 0;
        if (elf_cfi_cfa(s->elf, pc, &cfa_reg, &cfa_off) != 0) break;
        uint32_t base = (cfa_reg == 32) ? sp : y;
        uint32_t cfa  = (base + (uint32_t)cfa_off) & 0xffffu;

        uint32_t ra_word = 0, caller_y = 0;
        /* Return-address word spans [CFA-1, CFA] — the CIE's "RA at cfa-1" — as
         * the 2-byte word PC a `call` pushed, high byte at CFA-1, low at CFA. */
        {
            uint8_t rb[2];
            if (updi_mem_read(s->updi_fd, (cfa - 1u) & 0xffffu, rb, 2) < 0) break;
            ra_word = (uint32_t)rb[1] | ((uint32_t)rb[0] << 8);
        }
        /* Caller's saved Y: the callee prologue did `push r28; push r29`, so
         * r28 (Y-low) lands at CFA-2 and r29 (Y-high) at CFA-3. */
        {
            uint8_t yb[2];
            if (updi_mem_read(s->updi_fd, (cfa - 3u) & 0xffffu, yb, 2) >= 0)
                caller_y = (uint32_t)yb[1] | ((uint32_t)yb[0] << 8);
        }

        if (dbg) {
            uint8_t win[10];
            (void)updi_mem_read(s->updi_fd, (cfa - 6u) & 0xffffu, win, sizeof win);
            fprintf(stderr,
                "[unwind] f%d pc=0x%04x cfa=r%d+%d=0x%04x  win[cfa-6..+3]="
                "%02x %02x %02x %02x %02x %02x|%02x %02x %02x %02x  "
                "ra_word@cfa=0x%04x->pc=0x%04x  caller_y=0x%04x\n",
                n - 1, pc, cfa_reg, cfa_off, cfa,
                win[0],win[1],win[2],win[3],win[4],win[5],win[6],win[7],win[8],win[9],
                ra_word, ra_word << 1, caller_y);
        }

        uint32_t ra_byte = ra_word << 1;   /* AVR word PC -> byte address */
        if (ra_word == 0) break;
        if (flash_size != 0 && ra_byte >= flash_size) break;
        if (cfa == prev_cfa) break;        /* no progress — bail */

        prev_cfa = cfa;
        pc = ra_byte;
        sp = cfa;
        y  = caller_y;
        pcs[n++] = pc;
    }
    return n;
}

/* Phase 17 → 18: `stackTrace`.  Frame 0 from the live PC; deeper frames via
 * DWARF CFI unwinding (dap_unwind).  With no target (updi_fd < 0, unit tests)
 * or no DWARF it still returns one frame so the client has a valid stack. */
static int dap_handle_stack_trace(dap_session *s, long req_seq)
{
    uint32_t pcs[DAP_MAX_FRAMES];
    int n = dap_unwind(s, pcs, DAP_MAX_FRAMES);
    if (n <= 0) {
        uint32_t pc = 0;
        (void)(s->updi_fd >= 0 && updi_ocd_read_pc(s->updi_fd, &pc) == 0);
        pcs[0] = pc;
        n = 1;
    }

    char body[4096];
    size_t off = 0;
    int w = snprintf(body, sizeof body, "{\"stackFrames\":[");
    if (w > 0) off = (size_t)w;
    for (int i = 0; i < n; i++) {
        char frame[1024];
        dap_format_frame(s, i, pcs[i], frame, sizeof frame);
        w = snprintf(body + off, sizeof body - off, "%s%s", i ? "," : "", frame);
        if (w < 0 || (size_t)w >= sizeof body - off) break;
        off += (size_t)w;
    }
    (void)snprintf(body + off, sizeof body - off,
                   "],\"totalFrames\":%d}", n);
    return dap_send_response(s, req_seq, "stackTrace", true, body);
}

/* ── Breakpoints (Phase 18) ───────────────────────────────────────────────── */

void dap_bp_reset(dap_session *s)
{
    s->hw_bp_addr[0] = s->hw_bp_addr[1] = HW_BP_SLOT_EMPTY;
    s->hw_bp_pinned[0] = s->hw_bp_pinned[1] = false;
    bp_clear_all_sw(s->sw_bp);
    s->bp_mode  = RSP_BP_MODE_AUTO;
    s->pc_dirty = false;
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++) s->bps[i].in_use = false;
    s->next_bp_id = 1;
}

/* Uninstall + drop every DAP breakpoint whose source path equals `path`
 * (setBreakpoints replaces the full set for one source per call). */
static void dap_bp_clear_source(dap_session *s, const char *path)
{
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++) {
        if (!s->bps[i].in_use || strcmp(s->bps[i].source, path) != 0) continue;
        if (s->bps[i].verified && s->updi_fd >= 0)
            (void)bp_remove(s->updi_fd, s->hw_bp_addr, s->hw_bp_pinned,
                            s->sw_bp, s->bp_mode, &s->pc_dirty,
                            '0', s->bps[i].addr);
        s->bps[i].in_use = false;
    }
}

static int dap_bp_alloc(dap_session *s)
{
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++)
        if (!s->bps[i].in_use) return i;
    return -1;
}

/* setBreakpoints: resolve each source `line` to a code address via libdw and
 * install it through the shared breakpoint core (file:line source bps). */
static int dap_handle_set_breakpoints(dap_session *s, const char *msg,
                                      const dj_tok_t *t, long req_seq)
{
    char path[256] = "";
    int args = dj_member(msg, t, 0, "arguments");
    int srcobj = (args >= 0) ? dj_member(msg, t, args, "source") : -1;
    if (srcobj >= 0) {
        int p = dj_member(msg, t, srcobj, "path");
        if (p < 0) p = dj_member(msg, t, srcobj, "name");
        if (p >= 0) dj_strcpy(msg, t, p, path, sizeof path);
    }

    /* Replace this source's breakpoints wholesale. */
    dap_bp_clear_source(s, path);

    char body[1536];
    size_t off = 0;
    int    n   = snprintf(body, sizeof body, "{\"breakpoints\":[");
    if (n > 0) off = (size_t)n;

    int arr = (args >= 0) ? dj_member(msg, t, args, "breakpoints") : -1;
    int count = (arr >= 0 && t[arr].type == DJ_ARRAY) ? t[arr].size : 0;
    int elem = arr + 1;
    for (int k = 0; k < count; k++, elem = t[elem].next) {
        long line = 0;
        int lm = dj_member(msg, t, elem, "line");
        if (lm >= 0) (void)dj_long(msg, t, lm, &line);
        char cond[128] = "";
        int cm = dj_member(msg, t, elem, "condition");
        if (cm >= 0) dj_strcpy(msg, t, cm, cond, sizeof cond);

        uint32_t addr = 0;
        bool resolved = (s->elf != NULL && path[0] != '\0' &&
                         elf_line_to_addr(s->elf, path, (int)line, &addr) == 0);
        bool verified = false;
        int  id = s->next_bp_id;
        if (resolved && s->updi_fd >= 0) {
            int st = bp_insert(s->updi_fd, s->hw_bp_addr, s->hw_bp_pinned,
                               s->sw_bp, s->bp_mode, &s->pc_dirty, '0', addr);
            verified = (st == BP_OK);
        } else if (resolved) {
            verified = true;   /* unit-test path: resolved, no target to install */
        }

        int slot = dap_bp_alloc(s);
        if (slot >= 0) {
            s->bps[slot].id       = id;
            s->bps[slot].addr     = addr;
            s->bps[slot].line     = (int)line;
            s->bps[slot].verified = verified;
            s->bps[slot].in_use   = true;
            snprintf(s->bps[slot].source, sizeof s->bps[slot].source, "%s", path);
            snprintf(s->bps[slot].condition, sizeof s->bps[slot].condition,
                     "%s", cond);
            s->next_bp_id++;
        }

        int w = snprintf(body + off, sizeof body - off,
                         "%s{\"id\":%d,\"verified\":%s,\"line\":%ld}",
                         k ? "," : "", id, verified ? "true" : "false", line);
        if (w < 0 || (size_t)w >= sizeof body - off) break;   /* bound the reply */
        off += (size_t)w;
    }
    (void)snprintf(body + off, sizeof body - off, "]}");
    return dap_send_response(s, req_seq, "setBreakpoints", true, body);
}

/* setInstructionBreakpoints: install breakpoints at raw instruction addresses
 * (`instructionReference` + optional `offset`).  Replaces the full set of
 * instruction breakpoints (marked with line == -1) each call. */
static int dap_handle_set_instruction_breakpoints(dap_session *s, const char *msg,
                                                  const dj_tok_t *t, long req_seq)
{
    /* Drop any previously-installed instruction breakpoints (line == -1). */
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++) {
        if (!s->bps[i].in_use || s->bps[i].line != -1) continue;
        if (s->bps[i].verified && s->updi_fd >= 0)
            (void)bp_remove(s->updi_fd, s->hw_bp_addr, s->hw_bp_pinned,
                            s->sw_bp, s->bp_mode, &s->pc_dirty, '0', s->bps[i].addr);
        s->bps[i].in_use = false;
    }

    int args = dj_member(msg, t, 0, "arguments");
    char body[1536];
    size_t off = 0;
    int    n   = snprintf(body, sizeof body, "{\"breakpoints\":[");
    if (n > 0) off = (size_t)n;

    int arr = (args >= 0) ? dj_member(msg, t, args, "breakpoints") : -1;
    int count = (arr >= 0 && t[arr].type == DJ_ARRAY) ? t[arr].size : 0;
    int elem = arr + 1;
    for (int k = 0; k < count; k++, elem = t[elem].next) {
        char ref[32] = "";
        int rm = dj_member(msg, t, elem, "instructionReference");
        if (rm >= 0) dj_strcpy(msg, t, rm, ref, sizeof ref);
        long offw = 0;
        int om = dj_member(msg, t, elem, "offset");
        if (om >= 0) (void)dj_long(msg, t, om, &offw);

        uint32_t addr = 0;
        bool ok = (ref[0] != '\0');
        if (ok) addr = (uint32_t)(strtoul(ref, NULL, 0) + offw);

        bool verified = false;
        int  id = s->next_bp_id;
        if (ok && s->updi_fd >= 0) {
            int st = bp_insert(s->updi_fd, s->hw_bp_addr, s->hw_bp_pinned,
                               s->sw_bp, s->bp_mode, &s->pc_dirty, '0', addr);
            verified = (st == BP_OK);
        } else if (ok) {
            verified = true;   /* unit-test path: no target to install */
        }

        int slot = dap_bp_alloc(s);
        if (slot >= 0) {
            s->bps[slot].id        = id;
            s->bps[slot].addr      = addr;
            s->bps[slot].line      = -1;     /* marks an instruction breakpoint */
            s->bps[slot].verified  = verified;
            s->bps[slot].in_use    = true;
            s->bps[slot].source[0] = '\0';
            s->bps[slot].condition[0] = '\0';
            s->next_bp_id++;
        }

        int w = snprintf(body + off, sizeof body - off,
                         "%s{\"id\":%d,\"verified\":%s,"
                         "\"instructionReference\":\"0x%lx\"}",
                         k ? "," : "", id, verified ? "true" : "false",
                         (unsigned long)addr);
        if (w < 0 || (size_t)w >= sizeof body - off) break;
        off += (size_t)w;
    }
    (void)snprintf(body + off, sizeof body - off, "]}");
    return dap_send_response(s, req_seq, "setInstructionBreakpoints", true, body);
}

/* Emit a `stopped` event after a halt detected while running: if the live PC
 * matches an installed DAP breakpoint, report reason `breakpoint` with its
 * `hitBreakpointIds`; otherwise a plain `breakpoint` stop. */
static int dap_emit_stopped_breakpoint(dap_session *s)
{
    uint32_t pc = 0;
    int ids[DAP_MAX_BREAKPOINTS];
    int nids = 0;
    if (s->updi_fd >= 0 && updi_ocd_read_pc(s->updi_fd, &pc) == 0) {
        for (int i = 0; i < DAP_MAX_BREAKPOINTS && nids < DAP_MAX_BREAKPOINTS; i++)
            if (s->bps[i].in_use && s->bps[i].verified && s->bps[i].addr == pc)
                ids[nids++] = s->bps[i].id;
    }
    if (nids == 0)
        return dap_emit_stopped(s, "breakpoint");

    char   body[256];
    size_t off = 0;
    int    w = snprintf(body, sizeof body,
        "{\"reason\":\"breakpoint\",\"threadId\":1,\"allThreadsStopped\":true,"
        "\"hitBreakpointIds\":[");
    if (w > 0) off = (size_t)w;
    for (int i = 0; i < nids; i++) {
        w = snprintf(body + off, sizeof body - off, "%s%d", i ? "," : "", ids[i]);
        if (w < 0 || (size_t)w >= sizeof body - off) break;
        off += (size_t)w;
    }
    (void)snprintf(body + off, sizeof body - off, "]}");
    return dap_send_event(s, "stopped", body);
}

/* ── Conditional breakpoints + breakpoint-aware resume ────────────────────── */

/* Read the innermost frame's live PC and the register values a variable's DWARF
 * location may reference (Y pair, SP, and the CFA via .debug_frame). */
static int dap_frame0(dap_session *s, uint32_t *pc, ElfFrameRegs *fr)
{
    uint16_t sp16 = 0;
    uint8_t  r28 = 0, r29 = 0;
    if (s->updi_fd < 0 || s->elf == NULL) return -1;
    if (updi_ocd_read_pc (s->updi_fd, pc)       != 0) return -1;
    if (updi_ocd_read_sp (s->updi_fd, &sp16)    != 0) return -1;
    if (updi_ocd_read_gpr(s->updi_fd, 28, &r28) != 0) return -1;
    if (updi_ocd_read_gpr(s->updi_fd, 29, &r29) != 0) return -1;
    fr->y  = (uint32_t)r28 | ((uint32_t)r29 << 8);
    fr->sp = sp16;
    fr->cfa = 0;
    int reg = 0, off = 0;
    if (elf_cfi_cfa(s->elf, *pc, &reg, &off) == 0)
        fr->cfa = ((reg == 32 ? fr->sp : fr->y) + (uint32_t)off) & 0xffffu;
    return 0;
}

/* Evaluate a DAP breakpoint `condition` of the form `<ident> <op> <int>`
 * (op one of == != < <= > >=) against the live target: resolve `ident` to a
 * variable (local or global) via DWARF, read its value over OCD, and compare to
 * the integer literal.  Returns 1 (true -> stop), 0 (false -> resume), or -1
 * (cannot evaluate -> caller stops, so a bad condition never hides a hit). */
static int dap_eval_condition(dap_session *s, uint32_t pc,
                              const ElfFrameRegs *fr, const char *cond)
{
    const char *p = cond;
    while (*p == ' ' || *p == '\t') p++;

    char ident[64];
    int  k = 0;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return -1;
    while ((isalnum((unsigned char)*p) || *p == '_') && k < 63) ident[k++] = *p++;
    ident[k] = '\0';
    while (*p == ' ' || *p == '\t') p++;

    char op[3] = {0};
    if      (p[0] == '=' && p[1] == '=') { op[0]='='; op[1]='='; p += 2; }
    else if (p[0] == '!' && p[1] == '=') { op[0]='!'; op[1]='='; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { op[0]='<'; op[1]='='; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { op[0]='>'; op[1]='='; p += 2; }
    else if (p[0] == '<')                { op[0]='<';            p += 1; }
    else if (p[0] == '>')                { op[0]='>';            p += 1; }
    else return -1;
    while (*p == ' ' || *p == '\t') p++;

    char *end = NULL;
    long  rhs = strtol(p, &end, 0);
    if (end == p) return -1;

    uint32_t addr = 0;
    int      size = 2;
    bool     sg   = false;
    if (elf_var_addr(s->elf, pc, fr, ident, &addr, &size, &sg) != 0) return -1;

    uint8_t buf[4] = {0};
    if (updi_mem_read(s->updi_fd, addr & 0xffffu, buf, (size_t)size) < 0) return -1;
    long lhs = 0;
    for (int i = 0; i < size; i++) lhs |= (long)buf[i] << (8 * i);
    if (sg && size < 4 && (lhs & (1L << (size * 8 - 1))))
        lhs |= -(1L << (size * 8));     /* sign-extend a signed sub-word */

    if (!strcmp(op, "==")) return lhs == rhs;
    if (!strcmp(op, "!=")) return lhs != rhs;
    if (!strcmp(op, "<"))  return lhs <  rhs;
    if (!strcmp(op, "<=")) return lhs <= rhs;
    if (!strcmp(op, ">"))  return lhs >  rhs;
    if (!strcmp(op, ">=")) return lhs >= rhs;
    return -1;
}

/* Step over every breakpoint installed at the current PC and resume — the GDB
 * remove/single-step/insert dance — so a conditional breakpoint whose condition
 * was false (or a plain resume parked on a breakpoint) does not immediately
 * re-trigger.  Returns 0 on success. */
static int dap_resume_over_current(dap_session *s)
{
    uint32_t pc = 0;
    if (updi_ocd_read_pc(s->updi_fd, &pc) != 0) return -1;
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++)
        if (s->bps[i].in_use && s->bps[i].verified && s->bps[i].addr == pc)
            (void)bp_remove(s->updi_fd, s->hw_bp_addr, s->hw_bp_pinned,
                            s->sw_bp, s->bp_mode, &s->pc_dirty, '0', pc);
    if (bp_step_over(s->updi_fd, s->sw_bp, &s->pc_dirty) < 0) return -1;
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++)
        if (s->bps[i].in_use && s->bps[i].verified && s->bps[i].addr == pc)
            (void)bp_insert(s->updi_fd, s->hw_bp_addr, s->hw_bp_pinned,
                            s->sw_bp, s->bp_mode, &s->pc_dirty, '0', pc);
    return updi_run(s->updi_fd) < 0 ? -1 : 0;
}

/* Resume the target for `continue`: step over a breakpoint parked at the live
 * PC if there is one, else a plain run. */
static int dap_target_resume(dap_session *s)
{
    if (s->updi_fd < 0) return 0;
    uint32_t pc = 0;
    if (updi_ocd_read_pc(s->updi_fd, &pc) == 0)
        for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++)
            if (s->bps[i].in_use && s->bps[i].verified && s->bps[i].addr == pc)
                return dap_resume_over_current(s);
    return updi_run(s->updi_fd) < 0 ? -1 : 0;
}

/* Called when the target halts while running.  If every breakpoint at the live
 * PC is conditional and all conditions are false, step over and auto-resume —
 * returning true (the caller keeps polling).  Returns false (the caller emits
 * `stopped`) for a spontaneous halt, an unconditional breakpoint, or any
 * condition that is true or cannot be evaluated. */
static bool dap_conditional_skip(dap_session *s)
{
    if (s->updi_fd < 0) return false;
    uint32_t      pc = 0;
    ElfFrameRegs  fr = {0, 0, 0};
    if (dap_frame0(s, &pc, &fr) != 0) return false;

    bool any = false, stop = false;
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++) {
        if (!(s->bps[i].in_use && s->bps[i].verified && s->bps[i].addr == pc))
            continue;
        any = true;
        if (s->bps[i].condition[0] == '\0') { stop = true; continue; }
        if (dap_eval_condition(s, pc, &fr, s->bps[i].condition) != 0) stop = true;
    }
    if (!any || stop) return false;
    return dap_resume_over_current(s) == 0;
}

/* ── Request dispatch ─────────────────────────────────────────────────────── */

int dap_dispatch(dap_session *s, const char *msg, size_t len)
{
    dj_tok_t t[256];
    int      nt = dj_parse(msg, len, t, 256);
    if (nt < 1 || t[0].type != DJ_OBJECT)
        return 0;                               /* ignore non-object junk */

    long req_seq = 0;
    int  seqm = dj_member(msg, t, 0, "seq");
    if (seqm > 0)
        (void)dj_long(msg, t, seqm, &req_seq);

    int cmdm = dj_member(msg, t, 0, "command");
    if (cmdm < 0)
        return 0;                               /* not a request */
    char cmd[48];
    dj_strcpy(msg, t, cmdm, cmd, sizeof cmd);

    /* ── Lifecycle (Phase 16) ── */
    if (strcmp(cmd, "initialize") == 0) {
        /* Advertise only what the handshake needs; capabilities grow with the
         * Phase 17–19 features. */
        if (dap_send_response(s, req_seq, cmd, true,
                "{\"supportsConfigurationDoneRequest\":true,"
                "\"supportsInstructionBreakpoints\":true}") < 0)
            return -1;
        return dap_send_event(s, "initialized", NULL) < 0 ? -1 : 0;
    }
    if (strcmp(cmd, "launch") == 0 || strcmp(cmd, "attach") == 0)
        return dap_send_response(s, req_seq, cmd, true, NULL) < 0 ? -1 : 0;

    if (strcmp(cmd, "setExceptionBreakpoints") == 0)
        return dap_send_response(s, req_seq, cmd, true, NULL) < 0 ? -1 : 0;

    if (strcmp(cmd, "setBreakpoints") == 0)
        return dap_handle_set_breakpoints(s, msg, t, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "setInstructionBreakpoints") == 0)
        return dap_handle_set_instruction_breakpoints(s, msg, t, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "configurationDone") == 0) {
        if (s->updi_fd >= 0)
            (void)updi_halt(s->updi_fd);
        s->running = false;
        if (dap_send_response(s, req_seq, cmd, true, NULL) < 0)
            return -1;
        /* Reaching a stopped-at-entry state completes the lifecycle handshake. */
        return dap_emit_stopped(s, "entry") < 0 ? -1 : 0;
    }

    if (strcmp(cmd, "threads") == 0)
        /* The live CPU is the sole thread; FSM tasks remain introspection
         * (surfaced as a later refinement). */
        return dap_send_response(s, req_seq, cmd, true,
                "{\"threads\":[{\"id\":1,\"name\":\"cpu\"}]}") < 0 ? -1 : 0;

    /* ── Execution control + stop events (Phase 17) ── */
    if (strcmp(cmd, "continue") == 0) {
        if (dap_send_response(s, req_seq, cmd, true,
                "{\"allThreadsContinued\":true}") < 0)
            return -1;
        if (dap_send_event(s, "continued",
                "{\"threadId\":1,\"allThreadsContinued\":true}") < 0)
            return -1;
        if (s->updi_fd >= 0) {
            if (dap_target_resume(s) == 0)
                s->running = true;   /* dap_serve() polls for the halt */
            else
                return dap_emit_stopped(s, "breakpoint") < 0 ? -1 : 0;
        }
        return 0;
    }

    if (strcmp(cmd, "pause") == 0) {
        if (s->updi_fd >= 0)
            (void)updi_halt(s->updi_fd);
        s->running = false;
        if (dap_send_response(s, req_seq, cmd, true, NULL) < 0)
            return -1;
        return dap_emit_stopped(s, "pause") < 0 ? -1 : 0;
    }

    /* next / stepIn / stepOut map to a single OCD instruction step (the core
     * execution verb); source-line granularity and true step-over/step-out
     * are a later refinement.  Either way the resulting PC resolves to the
     * correct source line via the shallow stackTrace.                      */
    if (strcmp(cmd, "next") == 0 || strcmp(cmd, "stepIn") == 0 ||
        strcmp(cmd, "stepOut") == 0) {
        if (s->updi_fd >= 0)
            (void)updi_step(s->updi_fd);
        s->running = false;
        if (dap_send_response(s, req_seq, cmd, true, NULL) < 0)
            return -1;
        return dap_emit_stopped(s, "step") < 0 ? -1 : 0;
    }

    if (strcmp(cmd, "stackTrace") == 0)
        return dap_handle_stack_trace(s, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "scopes") == 0)
        /* Frame scopes (locals/registers) carry variables — Phase 19. */
        return dap_send_response(s, req_seq, cmd, true,
                "{\"scopes\":[]}") < 0 ? -1 : 0;

    if (strcmp(cmd, "disconnect") == 0 || strcmp(cmd, "terminate") == 0) {
        /* Remove all installed breakpoints from silicon before resuming, so
         * the target runs free (and is not left halted on an armed comparator
         * at the current PC).  Then let it run on detach. */
        if (s->updi_fd >= 0) {
            for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++) {
                if (s->bps[i].in_use && s->bps[i].verified)
                    (void)bp_remove(s->updi_fd, s->hw_bp_addr, s->hw_bp_pinned,
                                    s->sw_bp, s->bp_mode, &s->pc_dirty,
                                    '0', s->bps[i].addr);
                s->bps[i].in_use = false;
            }
            (void)updi_run(s->updi_fd);          /* let the target run on detach */
        }
        s->running = false;
        /* `terminate` asks the adapter to stop the debuggee; signal the
         * session end with a `terminated` event before the response.       */
        if (strcmp(cmd, "terminate") == 0)
            (void)dap_send_event(s, "terminated", NULL);
        (void)dap_send_response(s, req_seq, cmd, true, NULL);
        return 1;                                /* close the session */
    }

    /* Variables, memory, evaluate, and breakpoint installation: Phases 18–19. */
    return dap_send_error(s, req_seq, cmd,
                          "request not implemented yet (Phase 18+)") < 0 ? -1 : 0;
}

/* ── Server entry: accept one client, then dispatch on the event loop ─────── */

int dap_serve(int listen_fd, int updi_fd,
              ElfContext *elf, const AvrOsSymbolIndex *idx,
              FsmContext *fsm, volatile sig_atomic_t *quit, bool log)
{
    int cfd = rsp_accept(listen_fd);
    if (cfd < 0) {
        fprintf(stderr, "avrOSdb: DAP accept failed\n");
        return -1;
    }
    if (log)
        fprintf(stderr, "avrOSdb: DAP client connected\n");

    char *buf = malloc(DAP_MSG_MAX);
    if (buf == NULL) {
        rsp_close(cfd);
        return -1;
    }

    dap_session s = { .fd = cfd, .updi_fd = updi_fd, .elf = elf, .idx = idx,
                      .fsm = fsm, .log = log, .out_seq = 0, .running = false };
    dap_bp_reset(&s);   /* initialise breakpoint state (hw slots EMPTY, AUTO) */
    int rc = 0;
    while (quit == NULL || !*quit) {
        /* Wait for a readable client.  Tick faster while the target is running
         * (Phase 17) so a breakpoint/spontaneous halt surfaces promptly; idle
         * slower otherwise so the quit flag is honoured without a busy loop. */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(cfd, &rf);
        struct timeval tv = { 0, s.running ? 20000 : 200000 };
        int sel = select(cfd + 1, &rf, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            rc = -1; break;
        }
        if (sel == 0) {
            /* Idle tick: if the target was resumed via `continue`, probe the
             * OCD STOPPED status and emit `stopped` when it halts. */
            if (s.running && updi_fd >= 0) {
                int h = updi_ocd_poll_halted(updi_fd, 1);
                if (h == 0) {
                    /* A conditional breakpoint whose condition is false is
                     * stepped over and the target auto-resumed (still
                     * running); otherwise report the stop. */
                    if (dap_conditional_skip(&s)) {
                        /* resumed — keep polling */
                    } else {
                        s.running = false;
                        if (dap_emit_stopped_breakpoint(&s) < 0) { rc = -1; break; }
                    }
                } else if (h < 0) {
                    fprintf(stderr, "avrOSdb: DAP target poll error\n");
                    rc = -1; break;
                }
            }
            continue;
        }

        size_t mlen = 0;
        int    r = dap_read_message(cfd, buf, DAP_MSG_MAX, &mlen);
        if (r == 0) {
            if (log) fprintf(stderr, "avrOSdb: DAP client disconnected (EOF)\n");
            break;
        }
        if (r < 0) {
            fprintf(stderr, "avrOSdb: DAP read/framing error\n");
            rc = -1; break;
        }
        if (dap_dispatch(&s, buf, mlen) != 0)
            break;                               /* disconnect/terminate */
    }

    free(buf);
    rsp_close(cfd);
    return rc;
}
