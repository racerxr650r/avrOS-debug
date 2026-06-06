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

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
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

/* ── Server entry (scaffold — handlers land next on this branch) ──────────── */

int dap_serve(int listen_fd, int updi_fd,
              ElfContext *elf, const AvrOsSymbolIndex *idx,
              FsmContext *fsm, volatile sig_atomic_t *quit, bool log)
{
    (void)listen_fd; (void)updi_fd; (void)elf; (void)idx;
    (void)fsm; (void)quit; (void)log;

    fprintf(stderr,
            "avrOSdb: --dap selected. The DAP transport (framing + JSON codec) "
            "is in place; the accept/dispatch loop and lifecycle handlers are "
            "still under construction (Phase 16). Use --rsp (the default) for "
            "now.\n");
    return -1;
}
