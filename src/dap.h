/* src/dap.h — Debug Adapter Protocol (DAP) front-end (Phase 16+).
 *
 * A client-facing protocol front-end over the protocol-agnostic debug core
 * (debug_core.h), running **in parallel** to the GDB-RSP front-end in
 * gdb_rsp.c — selected at startup with `--dap` (`--rsp` is the default; see
 * main.c). It targets DAP-native editors, primarily VS Code, and is
 * automated-tested via headless Neovim + nvim-dap Lua scripts.
 *
 * Transport: `Content-Length: <n>\r\n\r\n<json>` JSON-RPC over the same TCP
 * listener and single select() event loop the RSP server uses.
 *
 * Phase 16 lands the foundation: this module, the `--dap`/`--rsp` mode
 * dispatch, the transport, and the lifecycle handshake
 * (initialize → launch/attach → configurationDone → disconnect). Execution
 * control + stop events (Phase 17), breakpoints + DWARF unwinding (Phase 18),
 * and variables/memory/evaluate (Phase 19) follow. See doc/SDP.md §8 and
 * doc/reference/dual-protocol-architecture.md.
 *
 * Layering: like gdb_rsp.c, this front-end translates wire framing to/from the
 * debug-core verbs and is the only layer that formats DAP replies; it never
 * reaches below the core (HLR-073).
 */
#ifndef AOD_DAP_H
#define AOD_DAP_H

#include <signal.h>     /* sig_atomic_t */
#include <stddef.h>     /* size_t */
#include <stdbool.h>

#include "debug_core.h" /* ElfContext, AvrOsSymbolIndex, FsmContext + core API */

/* ── DAP wire transport: Content-Length-framed JSON-RPC ────────────────────
 * The transport has two halves, both independent of the target so they are
 * unit-testable over a pipe/socketpair without hardware:
 *   1. message framing  (dap_read_message / dap_write_message)
 *   2. a small JSON codec (dj_* tokenizer + accessors + escaper)
 * The lifecycle/request handlers (Phase 16+) build on these.              */

/* Maximum DAP message body this build accepts/emits (bytes). DAP messages in
 * a debug session are small; this bounds the read buffer. */
#define DAP_MSG_MAX  65536u

/* ── JSON codec ────────────────────────────────────────────────────────────
 * A minimal recursive-descent tokenizer producing a flat, pre-order token
 * array. Chosen over vendoring jsmn so the parser is self-contained, in the
 * project's style, and directly unit-tested. */
typedef enum {
    DJ_OBJECT = 1, DJ_ARRAY, DJ_STRING, DJ_NUMBER, DJ_TRUE, DJ_FALSE, DJ_NULL
} dj_type_t;

typedef struct {
    dj_type_t type;
    int       start;  /* byte offset of token start; for STRING the first
                       * content byte (after the opening quote)            */
    int       end;    /* one past the last content byte (before closing quote
                       * for STRING)                                       */
    int       size;   /* OBJECT: member count; ARRAY: element count; else 0 */
    int       next;   /* index of the token after this token's whole subtree */
} dj_tok_t;

/* Parse JSON text [js, js+len) into up to `max` tokens. Returns the token
 * count (>=1) on success, or -1 on malformed input or token overflow. */
int    dj_parse(const char *js, size_t len, dj_tok_t *toks, int max);

/* Value-token index of object member `key`, or -1 if absent / not an object. */
int    dj_member(const char *js, const dj_tok_t *t, int obj, const char *key);

/* 1 iff token `idx` is a STRING whose (raw) content equals C-string `s`. */
int    dj_streq(const char *js, const dj_tok_t *t, int idx, const char *s);

/* Copy STRING token `idx` content into `buf` (decoding the common JSON
 * escapes), NUL-terminated and bounded by `cap`. Returns bytes written. */
size_t dj_strcpy(const char *js, const dj_tok_t *t, int idx,
                 char *buf, size_t cap);

/* Parse NUMBER token `idx` as a long into *out. Returns 0 on success, -1. */
int    dj_long(const char *js, const dj_tok_t *t, int idx, long *out);

/* Append `s` to `buf` as a JSON-escaped string body (no surrounding quotes),
 * bounded by `cap` (always NUL-terminates). Returns bytes written. */
size_t dj_escape(const char *s, char *buf, size_t cap);

/* ── Message framing ───────────────────────────────────────────────────────
 * Read one DAP message body off `fd` into `buf` (NUL-terminated), setting
 * *out_len. Returns 1 = message read, 0 = peer closed/EOF, -1 = error,
 * oversize, or malformed header. Reads exactly the framed body — never
 * over-reads into the next message. */
int dap_read_message(int fd, char *buf, size_t cap, size_t *out_len);

/* Write one DAP message: `Content-Length: <len>\r\n\r\n` + body. 0 / -1. */
int dap_write_message(int fd, const char *body, size_t len);

/* ── Session + request dispatch ────────────────────────────────────────────
 * One DAP session over a connected client fd; handlers drive the target via
 * the debug-core handles. Exposed so the dispatch is unit-testable over a
 * socketpair without the accept loop or hardware (set `updi_fd = -1` to skip
 * the UPDI side effects). */
typedef struct {
    int                     fd;       /* connected DAP client socket            */
    int                     updi_fd;  /* target UPDI link (-1 in unit tests)    */
    ElfContext             *elf;
    const AvrOsSymbolIndex *idx;
    FsmContext             *fsm;
    bool                    log;
    long                    out_seq;  /* outgoing message seq (pre-incremented) */
    bool                    running;  /* Phase 17: target resumed via `continue`;
                                       * dap_serve() polls the OCD STOPPED status
                                       * each idle tick and emits `stopped` when
                                       * the target halts (breakpoint / spontaneous). */
} dap_session;

/* Handle one decoded DAP message: parse, dispatch to the matching request
 * handler, and write the response (+ any events). Returns 0 to continue, 1 to
 * disconnect the client (disconnect/terminate), -1 on a write error. */
int dap_dispatch(dap_session *s, const char *msg, size_t len);

/* Serve the DAP front-end on the already-bound TCP listener `listen_fd` until
 * the client disconnects or `*quit` is set. The target UPDI link (`updi_fd`)
 * and the loaded ELF/DWARF (`elf`), avrOS symbol index (`idx`), and FSM
 * introspection context (`fsm`, may be NULL) are the debug-core handles the
 * DAP requests operate over. Returns 0 on clean shutdown, -1 on a fatal
 * setup error. */
int dap_serve(int listen_fd, int updi_fd,
              ElfContext *elf, const AvrOsSymbolIndex *idx,
              FsmContext *fsm, volatile sig_atomic_t *quit, bool log);

#endif /* AOD_DAP_H */
