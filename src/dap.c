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
#include "fsm_mapper.h" /* fsm_build_thread_list — avrosdb/fsmList */
#include "avros.h"      /* avros_read_events/queues — avrosdb/eventList/queueList */

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

#define DAP_OUT_MAX 16384u  /* fits the largest responses: a Registers scope
                             * (~35 vars), a deep stackTrace, or a wide
                             * variables/array expansion */

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
    /* Frame name: the enclosing function (DWARF), falling back to the raw PC
     * hex when no subprogram covers it (e.g. the C runtime above main). */
    char fname[128], nesc[260];
    int  have_func = (s->elf != NULL &&
                      elf_addr_to_func(s->elf, pc, fname, sizeof fname) == 0);
    if (have_func) {
        dj_escape(fname, nesc, sizeof nesc);
    } else {
        (void)snprintf(nesc, sizeof nesc, "0x%06lx", (unsigned long)pc);
    }

    /* Only attach a source object when the PC is inside a real function.  A PC
     * with no enclosing subprogram (the reset vector / C-runtime, or a stub) is
     * not a C source location — yet libdw's line table can still hand back a
     * stale row for it (e.g. a `--gc-sections`-discarded function whose
     * `.debug_line` entry was relocated to address 0), which would make VS Code
     * show a misleading file:line.  Gating on `have_func` suppresses that and
     * lets the client show the instruction address instead. */
    char file[256];
    int  line = 0;
    if (have_func && s->elf != NULL &&
        elf_addr_to_line(s->elf, pc, file, sizeof file, &line) == 0) {
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        char pesc[600], besc[200];
        dj_escape(file, pesc, sizeof pesc);
        dj_escape(base, besc, sizeof besc);
        (void)snprintf(out, cap,
            "{\"id\":%d,\"name\":\"%s\",\"line\":%d,\"column\":1,"
            "\"instructionPointerReference\":\"0x%lx\","
            "\"source\":{\"name\":\"%s\",\"path\":\"%s\"}}",
            id, nesc, line, (unsigned long)pc, besc, pesc);
    } else {
        (void)snprintf(out, cap,
            "{\"id\":%d,\"name\":\"%s\",\"line\":0,\"column\":1,"
            "\"instructionPointerReference\":\"0x%lx\"}",
            id, nesc, (unsigned long)pc);
    }
}

/* Phase 18/19: multi-frame stack unwind via .debug_frame CFI + the live target.
 *
 * For each frame we get the CFA rule (CFA = value(cfa_reg) + offset) from
 * elf_cfi_cfa(), where cfa_reg is the Y frame-pointer pair r28:r29 (28) or SP
 * (32).  AVR's fixed stack conventions then give the caller:
 *   - the 2-byte word return address sits at [CFA-1, CFA] (a `call` pushes the
 *     word PC); the code-space byte PC is that word << 1;
 *   - the caller's Y was saved by the callee prologue (push r28; push r29), so
 *     r28 (Y-low) lands at CFA-2 and r29 (Y-high) at CFA-3.
 * The caller's SP at its call site is the CFA.  We stop at an invalid return
 * address, where no CFI covers the PC (the C-runtime frame above main), when
 * the CFA stops advancing, or at DAP_MAX_FRAMES.  The byte offsets are AVR
 * conventions locked against avr-gdb (Appendix B.17).
 *
 * Fills s->frames[0..n-1] (frame 0 = innermost) with each frame's PC and its
 * register context (CFA / Y / SP — consumed by `scopes`), sets s->nframes, and
 * returns n (>= 1), or 0 if the live registers can't be read. */
static int dap_unwind(dap_session *s)
{
    uint32_t pc = 0, y;
    uint16_t sp16 = 0;
    uint8_t  r28 = 0, r29 = 0;

    s->nframes = 0;
    if (s->updi_fd < 0 || s->elf == NULL) return 0;
    if (updi_ocd_read_pc (s->updi_fd, &pc)      != 0) return 0;
    if (updi_ocd_read_sp (s->updi_fd, &sp16)    != 0) return 0;
    if (updi_ocd_read_gpr(s->updi_fd, 28, &r28) != 0) return 0;
    if (updi_ocd_read_gpr(s->updi_fd, 29, &r29) != 0) return 0;

    uint32_t sp = sp16;
    y = (uint32_t)r28 | ((uint32_t)r29 << 8);

    uint32_t   flash_size = s->elf->flash_size;
    const bool dbg = getenv("AVROSDB_DAP_UNWIND_LOG") != NULL;

    int      n = 0;
    uint32_t prev_cfa = 0;
    for (;;) {
        if (n >= DAP_MAX_FRAMES) break;

        int  cfa_reg = 0, cfa_off = 0;
        bool have_cfa = (elf_cfi_cfa(s->elf, pc, &cfa_reg, &cfa_off) == 0);
        uint32_t cfa = have_cfa
            ? (((cfa_reg == 32 ? sp : y) + (uint32_t)cfa_off) & 0xffffu) : 0;

        /* Record this frame and its register context. */
        s->frames[n].pc     = pc;
        s->frames[n].fr.cfa = cfa;
        s->frames[n].fr.y   = y;
        s->frames[n].fr.sp  = sp;
        n++;
        if (!have_cfa) break;            /* no CFI here — outermost reachable */

        uint8_t rb[2], yb[2];
        if (updi_mem_read(s->updi_fd, (cfa - 1u) & 0xffffu, rb, 2) < 0) break;
        uint32_t ra_word  = (uint32_t)rb[1] | ((uint32_t)rb[0] << 8);
        uint32_t caller_y = 0;
        if (updi_mem_read(s->updi_fd, (cfa - 3u) & 0xffffu, yb, 2) >= 0)
            caller_y = (uint32_t)yb[1] | ((uint32_t)yb[0] << 8);
        uint32_t ra_byte = ra_word << 1;

        if (dbg)
            fprintf(stderr, "[unwind] f%d pc=0x%04x cfa=r%d+%d=0x%04x "
                    "ra=0x%04x->0x%04x cy=0x%04x\n", n - 1, pc, cfa_reg, cfa_off,
                    cfa, ra_word, ra_byte, caller_y);

        if (ra_word == 0) break;
        if (flash_size != 0 && ra_byte >= flash_size) break;
        if (cfa == prev_cfa) break;      /* no progress — bail */

        prev_cfa = cfa;
        pc = ra_byte;
        sp = cfa;
        y  = caller_y;
    }
    s->nframes = n;
    return n;
}

/* Phase 17 → 18/19: `stackTrace`.  Frame 0 from the live PC; deeper frames via
 * DWARF CFI unwinding (dap_unwind), recording each frame's register context for
 * `scopes`.  With no target (updi_fd < 0, unit tests) or no DWARF it still
 * returns one frame so the client has a valid stack. */
static int dap_handle_stack_trace(dap_session *s, long req_seq)
{
    int n = dap_unwind(s);
    if (n <= 0) {
        uint32_t pc = 0;
        (void)(s->updi_fd >= 0 && updi_ocd_read_pc(s->updi_fd, &pc) == 0);
        s->frames[0].pc = pc;
        s->frames[0].fr.cfa = s->frames[0].fr.y = s->frames[0].fr.sp = 0;
        s->nframes = n = 1;
    }

    char body[4096];
    size_t off = 0;
    int w = snprintf(body, sizeof body, "{\"stackFrames\":[");
    if (w > 0) off = (size_t)w;
    for (int i = 0; i < n; i++) {
        char frame[1600];   /* fits id + function name + escaped file:line */
        dap_format_frame(s, i, s->frames[i].pc, frame, sizeof frame);
        w = snprintf(body + off, sizeof body - off, "%s%s", i ? "," : "", frame);
        if (w < 0 || (size_t)w >= sizeof body - off) break;
        off += (size_t)w;
    }
    (void)snprintf(body + off, sizeof body - off,
                   "],\"totalFrames\":%d}", n);
    return dap_send_response(s, req_seq, "stackTrace", true, body);
}

/* ── Scopes & Variables (Phase 19) ────────────────────────────────────────── */

/* elf_type_render memory-read callback over the target data space: SRAM/IO are
 * the low 16-bit data addresses; the mapped-flash window (data >= 0x8000, where
 * avr-gcc places `.rodata`) is routed through the UPDI flash mirror. */
static int dap_mem_cb(void *user, uint32_t addr, uint8_t *buf, int len)
{
    dap_session *s = (dap_session *)user;
    if (s->updi_fd < 0) return -1;
    uint32_t a = (addr >= 0x8000u) ? flash_to_updi(addr) : (addr & 0xffffu);
    return updi_mem_read(s->updi_fd, a, buf, (size_t)len) < 0 ? -1 : 0;
}

/* Allocate a variablesReference (1-based); returns 0 when the table is full
 * (DAP treats 0 as "no children"). */
static int dap_varref_alloc(dap_session *s, int kind, uint32_t pc,
                            const ElfFrameRegs *fr, uint32_t addr,
                            uint64_t type_off)
{
    if (s->next_varref < 1) s->next_varref = 1;
    if (s->next_varref >= DAP_MAX_VARREFS) return 0;
    int r = s->next_varref++;
    s->varrefs[r].kind     = kind;
    s->varrefs[r].pc       = pc;
    s->varrefs[r].addr     = addr;
    s->varrefs[r].type_off = type_off;
    if (fr) s->varrefs[r].fr = *fr;
    else    s->varrefs[r].fr.cfa = s->varrefs[r].fr.y = s->varrefs[r].fr.sp = 0;
    return r;
}

/* Invalidate all frame + variable handles (the target moved). */
static void dap_varref_reset(dap_session *s) { s->next_varref = 1; s->nframes = 0; }

/* Format one DAP Variable object for `v` (render its value; allocate a child
 * variablesReference when it is an aggregate). */
static int dap_format_var(dap_session *s, const ElfVar *v, char *out, size_t cap)
{
    char val[320];
    bool expandable = false;
    if (s->elf == NULL ||
        elf_type_render(s->elf, v->addr, v->type_off, dap_mem_cb, s,
                        val, sizeof val, &expandable) != 0)
        snprintf(val, sizeof val, "<unavailable>");
    int ref = expandable
        ? dap_varref_alloc(s, DAP_VR_AGGREGATE, 0, NULL, v->addr, v->type_off) : 0;
    char ne[140], ve[640];
    dj_escape(v->name, ne, sizeof ne);
    dj_escape(val, ve, sizeof ve);
    return snprintf(out, cap,
        "{\"name\":\"%s\",\"value\":\"%s\",\"variablesReference\":%d,"
        "\"memoryReference\":\"0x%x\"}", ne, ve, ref, v->addr);
}

/* `scopes`: per frame, a Locals scope, a Registers scope (innermost frame only
 * — the live CPU registers), and a Globals scope.  Each carries a
 * variablesReference the client expands with `variables`. */
static int dap_handle_scopes(dap_session *s, const char *msg,
                             const dj_tok_t *t, long req_seq)
{
    long frame_id = 0;
    int  args = dj_member(msg, t, 0, "arguments");
    int  fm   = (args >= 0) ? dj_member(msg, t, args, "frameId") : -1;
    if (fm >= 0) (void)dj_long(msg, t, fm, &frame_id);
    if (frame_id < 0 || frame_id >= s->nframes) frame_id = 0;

    uint32_t     pc = (s->nframes > 0) ? s->frames[frame_id].pc : 0;
    ElfFrameRegs fr = (s->nframes > 0) ? s->frames[frame_id].fr
                                       : (ElfFrameRegs){0, 0, 0};

    int locals  = dap_varref_alloc(s, DAP_VR_LOCALS,  pc, &fr,  0, 0);
    int globals = dap_varref_alloc(s, DAP_VR_GLOBALS, pc, NULL, 0, 0);
    int regs    = (frame_id == 0)
        ? dap_varref_alloc(s, DAP_VR_REGISTERS, pc, NULL, 0, 0) : 0;

    char   body[512];
    size_t off = (size_t)snprintf(body, sizeof body,
        "{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":%d,"
        "\"presentationHint\":\"locals\",\"expensive\":false}", locals);
    if (regs)
        off += (size_t)snprintf(body + off, sizeof body - off,
            ",{\"name\":\"Registers\",\"variablesReference\":%d,"
            "\"presentationHint\":\"registers\",\"expensive\":false}", regs);
    off += (size_t)snprintf(body + off, sizeof body - off,
        ",{\"name\":\"Globals\",\"variablesReference\":%d,\"expensive\":false}]}",
        globals);
    return dap_send_response(s, req_seq, "scopes", true, body);
}

/* Append the live-register list (r0–r31, SP, SREG, PC) to `body`. */
static size_t dap_append_registers(dap_session *s, char *body, size_t cap,
                                   size_t off, int *first)
{
    for (int i = 0; i < 32; i++) {
        uint8_t rv = 0;
        if (s->updi_fd >= 0) (void)updi_ocd_read_gpr(s->updi_fd, (uint8_t)i, &rv);
        int w = snprintf(body + off, cap - off,
            "%s{\"name\":\"r%d\",\"value\":\"0x%02x\",\"variablesReference\":0}",
            *first ? "" : ",", i, rv);
        if (w < 0 || (size_t)w >= cap - off) return off;
        off += (size_t)w; *first = 0;
    }
    uint16_t sp = 0; uint8_t sreg = 0; uint32_t pc = 0;
    if (s->updi_fd >= 0) {
        (void)updi_ocd_read_sp(s->updi_fd, &sp);
        (void)updi_ocd_read_sreg(s->updi_fd, &sreg);
        (void)updi_ocd_read_pc(s->updi_fd, &pc);
    }
    int w = snprintf(body + off, cap - off,
        ",{\"name\":\"SP\",\"value\":\"0x%04x\",\"variablesReference\":0}"
        ",{\"name\":\"SREG\",\"value\":\"0x%02x\",\"variablesReference\":0}"
        ",{\"name\":\"PC\",\"value\":\"0x%06x\",\"variablesReference\":0}",
        sp, sreg, pc);
    if (w > 0 && (size_t)w < cap - off) off += (size_t)w;
    return off;
}

/* `variables`: expand a scope (Locals/Registers/Globals) or an aggregate
 * (struct/union members, array elements) named by `variablesReference`. */
static int dap_handle_variables(dap_session *s, const char *msg,
                                const dj_tok_t *t, long req_seq)
{
    long vref = 0;
    int  args = dj_member(msg, t, 0, "arguments");
    int  vm   = (args >= 0) ? dj_member(msg, t, args, "variablesReference") : -1;
    if (vm >= 0) (void)dj_long(msg, t, vm, &vref);

    char   body[8192];
    size_t off   = (size_t)snprintf(body, sizeof body, "{\"variables\":[");
    int    first = 1;

    if (vref >= 1 && vref < DAP_MAX_VARREFS) {
        int kind = s->varrefs[vref].kind;
        if (kind == DAP_VR_REGISTERS) {
            off = dap_append_registers(s, body, sizeof body, off, &first);
        } else if (s->elf != NULL) {
            ElfVar vs[64];
            int nv;
            if (kind == DAP_VR_AGGREGATE)
                nv = elf_type_children(s->elf, s->varrefs[vref].addr,
                                       s->varrefs[vref].type_off, vs, 64);
            else
                nv = elf_var_enum(s->elf, s->varrefs[vref].pc, &s->varrefs[vref].fr,
                                  kind == DAP_VR_GLOBALS ? ELF_SCOPE_GLOBALS
                                                         : ELF_SCOPE_LOCALS,
                                  vs, 64);
            for (int i = 0; i < nv; i++) {
                char e[960];
                int  w = dap_format_var(s, &vs[i], e, sizeof e);
                if (w < 0) continue;
                int pw = snprintf(body + off, sizeof body - off, "%s%s",
                                  first ? "" : ",", e);
                if (pw < 0 || (size_t)pw >= sizeof body - off) break;
                off += (size_t)pw; first = 0;
            }
        }
    }
    (void)snprintf(body + off, sizeof body - off, "]}");
    return dap_send_response(s, req_seq, "variables", true, body);
}

/* ── evaluate / memory / setVariable (Phase 19) ──────────────────────────── */

static const char DAP_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t dap_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        char c[4] = { DAP_B64[(v >> 18) & 63], DAP_B64[(v >> 12) & 63],
                      (i + 1 < n) ? DAP_B64[(v >> 6) & 63] : '=',
                      (i + 2 < n) ? DAP_B64[v & 63] : '=' };
        if (o + 4 >= cap) break;
        for (int k = 0; k < 4; k++) out[o++] = c[k];
    }
    if (o < cap) out[o] = '\0';
    return o;
}

static int dap_b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t dap_b64_decode(const char *in, uint8_t *out, size_t cap)
{
    int q[4], qi = 0; size_t o = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') break;
        int v = dap_b64_val(*p);
        if (v < 0) continue;
        q[qi++] = v;
        if (qi == 4) {
            if (o < cap) out[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
            if (o < cap) out[o++] = (uint8_t)(((q[1] & 15) << 4) | (q[2] >> 2));
            if (o < cap) out[o++] = (uint8_t)(((q[2] & 3) << 6) | q[3]);
            qi = 0;
        }
    }
    if (qi >= 2 && o < cap) out[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
    if (qi >= 3 && o < cap) out[o++] = (uint8_t)(((q[1] & 15) << 4) | (q[2] >> 2));
    return o;
}

/* Resolve an `evaluate` expression — `ident` then a chain of `.field` /
 * `[index]` — at the given frame to (addr, type_off).  Returns 0 on success. */
static int dap_resolve_expr(dap_session *s, uint32_t pc, const ElfFrameRegs *fr,
                            const char *expr, uint32_t *addr, uint64_t *type_off)
{
    const char *p = expr;
    while (*p == ' ' || *p == '\t') p++;
    char id[64]; int k = 0;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return -1;
    while ((isalnum((unsigned char)*p) || *p == '_') && k < 63) id[k++] = *p++;
    id[k] = '\0';

    uint32_t a; uint64_t to;
    if (s->elf == NULL || elf_var_find(s->elf, pc, fr, id, &a, &to) != 0) return -1;

    while (*p) {
        ElfVar ch[64];
        if (*p == '.') {
            p++;
            char fn[64]; int j = 0;
            while ((isalnum((unsigned char)*p) || *p == '_') && j < 63) fn[j++] = *p++;
            fn[j] = '\0';
            int nc = elf_type_children(s->elf, a, to, ch, 64), hit = 0;
            for (int i = 0; i < nc; i++)
                if (strcmp(ch[i].name, fn) == 0) { a = ch[i].addr; to = ch[i].type_off; hit = 1; break; }
            if (!hit) return -1;
        } else if (*p == '[') {
            p++;
            long idx = strtol(p, (char **)&p, 0);
            if (*p == ']') p++;
            int nc = elf_type_children(s->elf, a, to, ch, 64);
            if (idx < 0 || idx >= nc) return -1;
            a = ch[idx].addr; to = ch[idx].type_off;
        } else if (*p == ' ' || *p == '\t') {
            p++;
        } else {
            return -1;
        }
    }
    *addr = a; *type_off = to;
    return 0;
}

/* `evaluate`: render a watch/REPL variable expression in a frame's context. */
static int dap_handle_evaluate(dap_session *s, const char *msg,
                               const dj_tok_t *t, long req_seq)
{
    int  args = dj_member(msg, t, 0, "arguments");
    char expr[160] = {0};
    long frame_id = 0;
    if (args >= 0) {
        int em = dj_member(msg, t, args, "expression");
        if (em >= 0) dj_strcpy(msg, t, em, expr, sizeof expr);
        int fm = dj_member(msg, t, args, "frameId");
        if (fm >= 0) (void)dj_long(msg, t, fm, &frame_id);
    }
    if (frame_id < 0 || frame_id >= s->nframes) frame_id = 0;
    uint32_t     pc = (s->nframes > 0) ? s->frames[frame_id].pc : 0;
    ElfFrameRegs fr = (s->nframes > 0) ? s->frames[frame_id].fr
                                       : (ElfFrameRegs){0, 0, 0};

    uint32_t addr = 0; uint64_t type_off = 0;
    if (expr[0] == '\0' ||
        dap_resolve_expr(s, pc, &fr, expr, &addr, &type_off) != 0)
        return dap_send_error(s, req_seq, "evaluate",
                              "not available") < 0 ? -1 : 0;

    char val[320]; bool ex = false;
    if (elf_type_render(s->elf, addr, type_off, dap_mem_cb, s,
                        val, sizeof val, &ex) != 0)
        return dap_send_error(s, req_seq, "evaluate", "unreadable") < 0 ? -1 : 0;
    int ref = ex ? dap_varref_alloc(s, DAP_VR_AGGREGATE, 0, NULL, addr, type_off) : 0;

    char ve[640], body[800];
    dj_escape(val, ve, sizeof ve);
    snprintf(body, sizeof body,
        "{\"result\":\"%s\",\"variablesReference\":%d,\"memoryReference\":\"0x%x\"}",
        ve, ref, addr);
    return dap_send_response(s, req_seq, "evaluate", true, body);
}

/* Parse a memoryReference ("0x….." / decimal) into a data-space address. */
static uint32_t dap_parse_memref(const char *ref) { return (uint32_t)strtoul(ref, NULL, 0); }

/* `readMemory`: base64 of `count` bytes from `memoryReference` + `offset`. */
static int dap_handle_read_memory(dap_session *s, const char *msg,
                                  const dj_tok_t *t, long req_seq)
{
    int  args = dj_member(msg, t, 0, "arguments");
    char ref[40] = {0}; long offset = 0, count = 0;
    if (args >= 0) {
        int rm = dj_member(msg, t, args, "memoryReference");
        if (rm >= 0) dj_strcpy(msg, t, rm, ref, sizeof ref);
        int om = dj_member(msg, t, args, "offset");  if (om >= 0) (void)dj_long(msg, t, om, &offset);
        int cm = dj_member(msg, t, args, "count");   if (cm >= 0) (void)dj_long(msg, t, cm, &count);
    }
    uint32_t addr = dap_parse_memref(ref) + (uint32_t)offset;
    if (count < 0) count = 0;
    if (count > 1024) count = 1024;

    uint8_t buf[1024];
    int got = 0;
    if (s->updi_fd >= 0 && count > 0) {
        uint32_t a = (addr >= 0x8000u) ? flash_to_updi(addr) : (addr & 0xffffu);
        if (updi_mem_read(s->updi_fd, a, buf, (size_t)count) >= 0) got = (int)count;
    }
    char b64[1400];
    dap_b64_encode(buf, (size_t)got, b64, sizeof b64);
    char body[1600];
    snprintf(body, sizeof body,
        "{\"address\":\"0x%x\",\"data\":\"%s\"%s}", addr, b64,
        got < count ? ",\"unreadableBytes\":1" : "");
    return dap_send_response(s, req_seq, "readMemory", true, body);
}

/* `writeMemory`: write base64 `data` to `memoryReference` + `offset`. */
static int dap_handle_write_memory(dap_session *s, const char *msg,
                                   const dj_tok_t *t, long req_seq)
{
    int  args = dj_member(msg, t, 0, "arguments");
    char ref[40] = {0}, data[1400] = {0}; long offset = 0;
    if (args >= 0) {
        int rm = dj_member(msg, t, args, "memoryReference");
        if (rm >= 0) dj_strcpy(msg, t, rm, ref, sizeof ref);
        int om = dj_member(msg, t, args, "offset"); if (om >= 0) (void)dj_long(msg, t, om, &offset);
        int dm = dj_member(msg, t, args, "data");   if (dm >= 0) dj_strcpy(msg, t, dm, data, sizeof data);
    }
    uint32_t addr = dap_parse_memref(ref) + (uint32_t)offset;
    uint8_t buf[1024];
    size_t  n = dap_b64_decode(data, buf, sizeof buf);
    bool ok = false;
    if (s->updi_fd >= 0 && n > 0 && addr < 0x8000u)
        ok = (updi_mem_write(s->updi_fd, addr & 0xffffu, buf, n) >= 0);
    char body[64];
    snprintf(body, sizeof body, "{\"bytesWritten\":%zu}", ok ? n : (size_t)0);
    return dap_send_response(s, req_seq, "writeMemory", ok, body);
}

/* `setVariable`: write a scalar back to a child of a Locals/Globals/aggregate
 * reference, named `name`, parsed from `value` (decimal or 0x-hex). */
static int dap_handle_set_variable(dap_session *s, const char *msg,
                                   const dj_tok_t *t, long req_seq)
{
    int  args = dj_member(msg, t, 0, "arguments");
    long vref = 0; char name[64] = {0}, value[64] = {0};
    if (args >= 0) {
        int vm = dj_member(msg, t, args, "variablesReference");
        if (vm >= 0) (void)dj_long(msg, t, vm, &vref);
        int nm = dj_member(msg, t, args, "name");  if (nm >= 0) dj_strcpy(msg, t, nm, name, sizeof name);
        int lm = dj_member(msg, t, args, "value"); if (lm >= 0) dj_strcpy(msg, t, lm, value, sizeof value);
    }
    if (vref < 1 || vref >= DAP_MAX_VARREFS || s->elf == NULL)
        return dap_send_error(s, req_seq, "setVariable", "no such variable") < 0 ? -1 : 0;

    /* Locate the named child in the reference to get its address + type. */
    ElfVar vs[64]; int nv; int kind = s->varrefs[vref].kind;
    if (kind == DAP_VR_AGGREGATE)
        nv = elf_type_children(s->elf, s->varrefs[vref].addr, s->varrefs[vref].type_off, vs, 64);
    else
        nv = elf_var_enum(s->elf, s->varrefs[vref].pc, &s->varrefs[vref].fr,
                          kind == DAP_VR_GLOBALS ? ELF_SCOPE_GLOBALS : ELF_SCOPE_LOCALS, vs, 64);
    ElfVar *target = NULL;
    for (int i = 0; i < nv; i++) if (strcmp(vs[i].name, name) == 0) { target = &vs[i]; break; }
    if (target == NULL)
        return dap_send_error(s, req_seq, "setVariable", "no such variable") < 0 ? -1 : 0;

    /* Write the new scalar little-endian, at the type's byte width. */
    long newval = strtol(value, NULL, 0);
    int  wsize  = elf_type_size(s->elf, target->type_off);
    uint8_t b[4];
    for (int i = 0; i < wsize; i++) b[i] = (uint8_t)((newval >> (8 * i)) & 0xff);
    bool ok = (s->updi_fd >= 0 && target->addr < 0x8000u &&
               updi_mem_write(s->updi_fd, target->addr & 0xffffu, b, (size_t)wsize) >= 0);

    char val[320]; bool ex = false;
    if (ok) elf_type_render(s->elf, target->addr, target->type_off, dap_mem_cb, s, val, sizeof val, &ex);
    else    snprintf(val, sizeof val, "<write failed>");
    char ve[640], body[720];
    dj_escape(val, ve, sizeof ve);
    snprintf(body, sizeof body, "{\"value\":\"%s\",\"variablesReference\":0}", ve);
    return dap_send_response(s, req_seq, "setVariable", ok, body);
}

/* ── avrOS introspection custom requests (Phase 21) ──────────────────────────
 *
 * `avrosdb/fsmList`, `avrosdb/eventList`, `avrosdb/queueList` surface the avrOS
 * runtime tables (the same data as `monitor avros tasks/events/queues`) as JSON
 * for the VS Code avrOS view.  They reuse the shared FSM/monitor readers and
 * the symbol index / FSM context already on the session; with no target or no
 * avrOS tables they return an empty list (never an error).  Non-intrusive —
 * background UPDI reads, no CPU halt. */

static int dap_handle_fsm_list(dap_session *s, long req_seq)
{
    char   body[4096];
    size_t off   = (size_t)snprintf(body, sizeof body, "{\"fsms\":[");
    int    first = 1;
    if (s->updi_fd >= 0 && s->idx != NULL && s->fsm != NULL &&
        fsm_build_thread_list(s->fsm, s->idx, s->updi_fd) >= 0) {
        for (int i = 0; i < s->fsm->thread_count; i++) {
            const FsmThread *t = &s->fsm->threads[i];
            char ne[80], se[80];
            dj_escape(t->name, ne, sizeof ne);
            dj_escape(t->state_name, se, sizeof se);
            int w = snprintf(body + off, sizeof body - off,
                "%s{\"id\":%d,\"name\":\"%s\",\"state\":\"%s\",\"active\":%s}",
                first ? "" : ",", t->gdb_id, ne, se,
                t->is_active ? "true" : "false");
            if (w < 0 || (size_t)w >= sizeof body - off) break;
            off += (size_t)w; first = 0;
        }
    }
    (void)snprintf(body + off, sizeof body - off, "]}");
    return dap_send_response(s, req_seq, "avrosdb/fsmList", true, body);
}

static int dap_handle_event_list(dap_session *s, long req_seq)
{
    AvrosEvent ev[256];
    int n = (s->updi_fd >= 0 && s->idx != NULL)
        ? avros_read_events(s->idx, s->updi_fd, ev,
                            (int)(sizeof ev / sizeof ev[0])) : 0;
    if (n < 0) n = 0;

    char   body[4096];
    size_t off = (size_t)snprintf(body, sizeof body, "{\"events\":[");
    for (int i = 0; i < n; i++) {
        char ne[80];
        dj_escape(ev[i].name, ne, sizeof ne);
        int w;
        if (ev[i].has_status)
            w = snprintf(body + off, sizeof body - off,
                "%s{\"name\":\"%s\",\"status\":%u}", i ? "," : "", ne,
                ev[i].status);
        else
            w = snprintf(body + off, sizeof body - off,
                "%s{\"name\":\"%s\",\"status\":null}", i ? "," : "", ne);
        if (w < 0 || (size_t)w >= sizeof body - off) break;
        off += (size_t)w;
    }
    (void)snprintf(body + off, sizeof body - off, "]}");
    return dap_send_response(s, req_seq, "avrosdb/eventList", true, body);
}

static int dap_handle_queue_list(dap_session *s, long req_seq)
{
    AvrosQueue q[256];
    int n = (s->updi_fd >= 0 && s->idx != NULL)
        ? avros_read_queues(s->idx, s->updi_fd, q,
                            (int)(sizeof q / sizeof q[0])) : 0;
    if (n < 0) n = 0;

    char   body[4096];
    size_t off = (size_t)snprintf(body, sizeof body, "{\"queues\":[");
    for (int i = 0; i < n; i++) {
        int w = snprintf(body + off, sizeof body - off,
            "%s{\"id\":%d,\"capacity\":%u,\"elemSize\":%u}",
            i ? "," : "", i, q[i].capacity, q[i].elem_size);
        if (w < 0 || (size_t)w >= sizeof body - off) break;
        off += (size_t)w;
    }
    (void)snprintf(body + off, sizeof body - off, "]}");
    return dap_send_response(s, req_seq, "avrosdb/queueList", true, body);
}

/* ── source request ──────────────────────────────────────────────────────────
 *
 * VS Code falls back to the DAP `source` request when it cannot open a frame's
 * `Source.path` itself.  stackTrace now emits absolute paths (LLR-ELF-13), so a
 * source present on the debug host is opened directly and this request is not
 * issued; we still serve it here as a robust fallback, reading
 * `arguments.source.path` from the host filesystem.  A file that is absent, or
 * too large to frame under DAP_MSG_MAX, gets a clean `success:false` rather than
 * a stub error. */
static int dap_handle_source(dap_session *s, const char *msg,
                             const dj_tok_t *t, long req_seq)
{
    char path[512] = "";
    int  args   = dj_member(msg, t, 0, "arguments");
    int  srcobj = (args >= 0) ? dj_member(msg, t, args, "source") : -1;
    if (srcobj >= 0) {
        int p = dj_member(msg, t, srcobj, "path");
        if (p >= 0) dj_strcpy(msg, t, p, path, sizeof path);
    }

    FILE *fp = (path[0] != '\0') ? fopen(path, "rb") : NULL;
    if (fp == NULL)
        return dap_send_error(s, req_seq, "source",
            "source file is not available on the debug host") < 0 ? -1 : 0;

    enum { SRC_RAW_MAX = 56u * 1024u };
    char *raw = malloc(SRC_RAW_MAX + 1u);
    if (raw == NULL) {
        fclose(fp);
        return dap_send_error(s, req_seq, "source", "out of memory") < 0 ? -1 : 0;
    }
    size_t n        = fread(raw, 1, SRC_RAW_MAX, fp);
    int    overflow = (fgetc(fp) != EOF);          /* more bytes than the cap */
    fclose(fp);
    raw[n] = '\0';
    if (overflow) {
        free(raw);
        return dap_send_error(s, req_seq, "source",
            "source too large to stream over DAP; open it from the workspace")
            < 0 ? -1 : 0;
    }

    /* Worst-case JSON escape is 6 bytes per input byte (\uXXXX). */
    size_t esccap = n * 6u + 1u;
    char  *esc    = malloc(esccap);
    char  *out    = malloc(DAP_MSG_MAX);
    if (esc == NULL || out == NULL) {
        free(raw); free(esc); free(out);
        return dap_send_error(s, req_seq, "source", "out of memory") < 0 ? -1 : 0;
    }
    dj_escape(raw, esc, esccap);
    free(raw);

    int w = snprintf(out, DAP_MSG_MAX,
        "{\"seq\":%ld,\"type\":\"response\",\"request_seq\":%ld,"
        "\"success\":true,\"command\":\"source\",\"body\":{\"content\":\"%s\"}}",
        dap_next_seq(s), req_seq, esc);
    free(esc);
    if (w < 0 || (size_t)w >= DAP_MSG_MAX) {
        free(out);
        return dap_send_error(s, req_seq, "source",
            "source too large to stream over DAP; open it from the workspace")
            < 0 ? -1 : 0;
    }
    int rc = dap_write_message(s->fd, out, (size_t)w);
    free(out);
    return rc < 0 ? -1 : 0;
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

/* ── Source-line stepping (HLR-078) ───────────────────────────────────────────
 *
 * VS Code sends next/stepIn/stepOut with no address range — unlike GDB, which
 * computes the source line's range and drives the server's `vCont;r`.  So the
 * DAP front-end orchestrates source-line stepping itself, but over the SAME
 * core primitives the GDB-RSP range-step uses: `bp_step_range()` single-steps
 * within a line's address range (`elf_line_range`), and the reserved stepping
 * comparator (`RSP_HW_BP_STEP_SLOT`) runs to a return address to step over / out
 * of calls without touching user breakpoints. */
#define DAP_STEP_LINE_MAX 4096      /* max line-table rows traversed per step   */
#define DAP_RUN_TO_POLLS  1200      /* 1200 × 50 ms = 60 s run-to-return budget  */

static int dap_pc_line(dap_session *s, uint32_t pc, char *file, size_t cap, int *line)
{
    if (s->elf == NULL) return -1;
    return elf_addr_to_line(s->elf, pc, file, cap, line);
}

/* Run the target until it halts at `target` with SP >= `min_sp` (so a recursive
 * re-entry of the same address at a deeper level is skipped), via the reserved
 * stepping comparator so user breakpoints are untouched.  Returns 0 when it
 * reaches the target (or stops at another breakpoint), -1 on a UPDI error or a
 * poll timeout (target left halted wherever it stopped). */
static int dap_run_to(dap_session *s, uint32_t target, uint16_t min_sp)
{
    if (updi_ocd_set_hw_bp(s->updi_fd, RSP_HW_BP_STEP_SLOT, target) < 0)
        return -1;
    int rc = -1;
    for (int run = 0; run < 64; run++) {
        if (updi_run(s->updi_fd) < 0) break;
        /* updi_ocd_poll_halted(): 0 = halted, 1 = still running (timeout),
         * -1 = UPDI error. */
        int halted = 0;
        for (int w = 0; w < DAP_RUN_TO_POLLS; w++) {
            int h = updi_ocd_poll_halted(s->updi_fd, 50);
            if (h < 0) { halted = -1; break; }
            if (h == 0) { halted = 1; break; }   /* STOPPED bit set */
            /* h > 0: still running within the budget → keep polling */
        }
        if (halted != 1) { (void)updi_halt(s->updi_fd); break; }
        uint32_t pc = 0; uint16_t sp = 0;
        (void)updi_ocd_read_pc(s->updi_fd, &pc);
        (void)updi_ocd_read_sp(s->updi_fd, &sp);
        if (pc != target)  { rc = 0; break; }   /* stopped at another (user) bp */
        if (sp >= min_sp)  { rc = 0; break; }    /* returned to our frame */
        /* same address but a deeper frame (recursion) → run again */
    }
    (void)updi_ocd_clear_hw_bp(s->updi_fd, RSP_HW_BP_STEP_SLOT);
    return rc;
}

/* stepIn: advance to a different source line, descending into calls. */
static void dap_step_line(dap_session *s)
{
    uint32_t pc = 0;
    if (updi_ocd_read_pc(s->updi_fd, &pc) < 0) return;
    char f0[256] = ""; int l0 = -1;
    int have0 = (dap_pc_line(s, pc, f0, sizeof f0, &l0) == 0);

    for (int g = 0; g < DAP_STEP_LINE_MAX; g++) {
        uint32_t lo = 0, hi = 0;
        if (s->elf != NULL && elf_line_range(s->elf, pc, &lo, &hi) == 0) {
            if (bp_step_range(s->updi_fd, lo, hi, NULL, NULL) < 0) return;
        } else if (updi_step(s->updi_fd) < 0) {
            return;                          /* no line info → single instr step */
        }
        if (updi_ocd_read_pc(s->updi_fd, &pc) < 0) return;
        char f[256] = ""; int l = -1;
        if (dap_pc_line(s, pc, f, sizeof f, &l) == 0
            && (!have0 || l != l0 || strcmp(f, f0) != 0))
            return;                          /* reached a different source line */
        /* same line (multi-row) or no line yet → keep stepping */
    }
}

/* next: like stepIn, but step OVER calls — run to the return rather than
 * single-stepping through (so stepping over a delay/loop is instant). */
static void dap_step_over(dap_session *s)
{
    uint32_t pc = 0; uint16_t sp0 = 0;
    if (updi_ocd_read_pc(s->updi_fd, &pc) < 0) return;
    if (updi_ocd_read_sp(s->updi_fd, &sp0) < 0) return;
    char f0[256] = ""; int l0 = -1;
    int have0 = (dap_pc_line(s, pc, f0, sizeof f0, &l0) == 0);
    char fn0[128] = "";
    int havef0 = (s->elf != NULL && elf_addr_to_func(s->elf, pc, fn0, sizeof fn0) == 0);

    for (int g = 0; g < DAP_STEP_LINE_MAX; g++) {
        uint32_t lo = 0, hi = 0;
        if (s->elf != NULL && elf_line_range(s->elf, pc, &lo, &hi) == 0) {
            if (bp_step_range(s->updi_fd, lo, hi, NULL, NULL) < 0) return;
        } else if (updi_step(s->updi_fd) < 0) {
            return;
        }
        uint32_t npc = 0; uint16_t sp = 0;
        if (updi_ocd_read_pc(s->updi_fd, &npc) < 0) return;
        (void)updi_ocd_read_sp(s->updi_fd, &sp);

        /* Descended into a call? (stack grew AND we left our function.)  Run to
         * the return address in our frame instead of stepping through the callee. */
        char fn[128] = "";
        int havef = (s->elf != NULL && elf_addr_to_func(s->elf, npc, fn, sizeof fn) == 0);
        if (sp < sp0 && (!havef0 || !havef || strcmp(fn, fn0) != 0)) {
            if (dap_unwind(s) >= 2) {        /* frames[1] = our frame (the return) */
                if (dap_run_to(s, s->frames[1].pc, sp0) != 0) return;
                if (updi_ocd_read_pc(s->updi_fd, &pc) < 0) return;
                continue;                    /* back in our frame; re-evaluate */
            }
            return;                          /* no unwind → stop here */
        }
        pc = npc;
        char f[256] = ""; int l = -1;
        if (dap_pc_line(s, pc, f, sizeof f, &l) == 0
            && (!have0 || l != l0 || strcmp(f, f0) != 0))
            return;
    }
}

/* stepOut: run until the current function returns to its caller. */
static void dap_step_out(dap_session *s)
{
    if (dap_unwind(s) < 2) { (void)updi_step(s->updi_fd); return; }  /* no caller */
    uint32_t ret    = s->frames[1].pc;            /* caller's return PC */
    uint16_t min_sp = (uint16_t)s->frames[0].fr.cfa;  /* SP once we return */
    (void)dap_run_to(s, ret, min_sp);
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
                "\"supportsInstructionBreakpoints\":true,"
                "\"supportsEvaluateForHovers\":true,"
                "\"supportsReadMemoryRequest\":true,"
                "\"supportsWriteMemoryRequest\":true,"
                "\"supportsSetVariable\":true}") < 0)
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
        dap_varref_reset(s);         /* frame/var handles go stale on resume */
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

    /* Source-line stepping (HLR-078): stepIn advances one source line into
     * calls; next steps over calls (run-to-return); stepOut runs to the caller.
     * All are built on the shared bp_step_range() core + the reserved stepping
     * comparator, so a line of several instructions is one step and a stepped-
     * over call (even a delay/loop) returns at once. */
    if (strcmp(cmd, "stepIn") == 0 || strcmp(cmd, "next") == 0 ||
        strcmp(cmd, "stepOut") == 0) {
        dap_varref_reset(s);         /* frame/var handles go stale on step */
        if (s->updi_fd >= 0) {
            if (strcmp(cmd, "stepIn") == 0)       dap_step_line(s);
            else if (strcmp(cmd, "next") == 0)    dap_step_over(s);
            else                                  dap_step_out(s);
        }
        s->running = false;
        if (dap_send_response(s, req_seq, cmd, true, NULL) < 0)
            return -1;
        return dap_emit_stopped(s, "step") < 0 ? -1 : 0;
    }

    if (strcmp(cmd, "stackTrace") == 0)
        return dap_handle_stack_trace(s, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "scopes") == 0)
        return dap_handle_scopes(s, msg, t, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "variables") == 0)
        return dap_handle_variables(s, msg, t, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "evaluate") == 0)
        return dap_handle_evaluate(s, msg, t, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "readMemory") == 0)
        return dap_handle_read_memory(s, msg, t, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "writeMemory") == 0)
        return dap_handle_write_memory(s, msg, t, req_seq) < 0 ? -1 : 0;

    if (strcmp(cmd, "setVariable") == 0)
        return dap_handle_set_variable(s, msg, t, req_seq) < 0 ? -1 : 0;

    /* avrOS introspection custom requests (Phase 21). */
    if (strcmp(cmd, "avrosdb/fsmList") == 0)
        return dap_handle_fsm_list(s, req_seq) < 0 ? -1 : 0;
    if (strcmp(cmd, "avrosdb/eventList") == 0)
        return dap_handle_event_list(s, req_seq) < 0 ? -1 : 0;
    if (strcmp(cmd, "avrosdb/queueList") == 0)
        return dap_handle_queue_list(s, req_seq) < 0 ? -1 : 0;

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

    if (strcmp(cmd, "source") == 0)
        return dap_handle_source(s, msg, t, req_seq);

    /* Any request we do not model: a clean success:false (never a hang). */
    return dap_send_error(s, req_seq, cmd,
                          "request not supported by avrOSdb") < 0 ? -1 : 0;
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
