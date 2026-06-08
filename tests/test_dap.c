/* tests/test_dap.c — Unit tests for the DAP wire transport (Phase 16):
 * the JSON codec (dj_* tokenizer + accessors + escaper) and the
 * Content-Length message framing. Both are target-independent and exercised
 * here without hardware (framing over an anonymous pipe). */
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "unity.h"
#include "dap.h"

/* dap.c references these for its accept loop + lifecycle side effects; this
 * suite links only dap.c and drives the codec/framing/dispatch directly, never
 * the real TCP/UPDI, so stub them. (Test objects are built with
 * -Wno-missing-prototypes.) `rsp_accept` hands dap_serve() a test-controlled
 * fd (a pre-connected socketpair end) so the accept/dispatch loop is
 * exercisable without a real listener. */
static int g_accept_fd = -1;
int  rsp_accept(int listen_fd)        { (void)listen_fd; return g_accept_fd; }
void rsp_close(int fd)                { (void)fd; }   /* the test owns the fds */
int  updi_halt(int fd)                { (void)fd; return 0; }
int  updi_run(int fd)                 { (void)fd; return 0; }
int  updi_step(int fd)                { (void)fd; return 0; }
int  updi_ocd_poll_halted(int fd, int t) { (void)fd; (void)t; return 0; }
int  updi_ocd_read_pc(int fd, uint32_t *a) { (void)fd; if (a) *a = 0; return 0; }
/* Dispatch tests run with updi_fd = -1 and elf = NULL, so the stackTrace
 * handler never calls into elf_addr_to_line; this stub satisfies the linker. */
int  elf_addr_to_line(const ElfContext *c, uint32_t a, char *f, size_t cap, int *ln)
{ (void)c; (void)a; (void)f; (void)cap; (void)ln; return -1; }

void setUp(void)    {}
void tearDown(void) {}

/* ── JSON codec ───────────────────────────────────────────────────────────── */

void test_dj_parse_extracts_request_fields(void)
{
    const char *js =
        "{\"seq\":3,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{\"clientID\":\"vscode\",\"linesStartAt1\":true}}";
    dj_tok_t t[64];
    int n = dj_parse(js, strlen(js), t, 64);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(DJ_OBJECT, t[0].type);

    int cmd = dj_member(js, t, 0, "command");
    TEST_ASSERT_TRUE(cmd > 0);
    TEST_ASSERT_TRUE(dj_streq(js, t, cmd, "initialize"));

    int seq = dj_member(js, t, 0, "seq");
    TEST_ASSERT_TRUE(seq > 0);
    long sv = 0;
    TEST_ASSERT_EQUAL_INT(0, dj_long(js, t, seq, &sv));
    TEST_ASSERT_EQUAL_INT(3, (int)sv);

    int args = dj_member(js, t, 0, "arguments");
    TEST_ASSERT_TRUE(args > 0);
    TEST_ASSERT_EQUAL_INT(DJ_OBJECT, t[args].type);

    int client = dj_member(js, t, args, "clientID");
    TEST_ASSERT_TRUE(client > 0);
    char name[32];
    dj_strcpy(js, t, client, name, sizeof name);
    TEST_ASSERT_EQUAL_STRING("vscode", name);

    int lines = dj_member(js, t, args, "linesStartAt1");
    TEST_ASSERT_TRUE(lines > 0);
    TEST_ASSERT_EQUAL_INT(DJ_TRUE, t[lines].type);

    /* absent member */
    TEST_ASSERT_EQUAL_INT(-1, dj_member(js, t, 0, "nope"));
}

void test_dj_parse_handles_arrays_and_nesting(void)
{
    const char *js = "{\"a\":[1,2,3],\"b\":{\"c\":\"x\"}}";
    dj_tok_t t[64];
    int n = dj_parse(js, strlen(js), t, 64);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    int a = dj_member(js, t, 0, "a");
    TEST_ASSERT_TRUE(a > 0);
    TEST_ASSERT_EQUAL_INT(DJ_ARRAY, t[a].type);
    TEST_ASSERT_EQUAL_INT(3, t[a].size);

    int b = dj_member(js, t, 0, "b");
    TEST_ASSERT_TRUE(b > 0);
    int c = dj_member(js, t, b, "c");
    TEST_ASSERT_TRUE(c > 0);
    char v[8];
    dj_strcpy(js, t, c, v, sizeof v);
    TEST_ASSERT_EQUAL_STRING("x", v);
}

void test_dj_strcpy_decodes_escapes(void)
{
    /* JSON source: {"p":"a\\b\"c\n"}  → value bytes: a \ b " c <LF> */
    const char *js = "{\"p\":\"a\\\\b\\\"c\\n\"}";
    dj_tok_t t[16];
    TEST_ASSERT_GREATER_THAN_INT(0, dj_parse(js, strlen(js), t, 16));
    int p = dj_member(js, t, 0, "p");
    TEST_ASSERT_TRUE(p > 0);
    char out[16];
    dj_strcpy(js, t, p, out, sizeof out);
    const char expected[] = { 'a', '\\', 'b', '"', 'c', '\n', '\0' };
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_dj_escape_quotes_backslash_and_controls(void)
{
    const char in[] = { 'a', '"', 'b', '\\', 'c', '\n', '\0' };
    char out[32];
    dj_escape(in, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\n", out);
}

void test_dj_parse_rejects_malformed(void)
{
    dj_tok_t t[16];
    TEST_ASSERT_EQUAL_INT(-1, dj_parse("{\"x\":}", 6, t, 16));      /* no value */
    TEST_ASSERT_EQUAL_INT(-1, dj_parse("{\"x\"", 4, t, 16));        /* truncated */
    TEST_ASSERT_EQUAL_INT(-1, dj_parse("[1,2", 4, t, 16));          /* unterminated array */
}

void test_dj_parse_respects_token_cap(void)
{
    const char *js = "[1,2,3,4,5,6,7,8]";
    dj_tok_t t[4];               /* far too few tokens */
    TEST_ASSERT_EQUAL_INT(-1, dj_parse(js, strlen(js), t, 4));
}

/* ── Message framing ──────────────────────────────────────────────────────── */

void test_dap_framing_round_trip_over_pipe(void)
{
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(fds));

    const char *m1 = "{\"seq\":1,\"command\":\"initialize\"}";
    const char *m2 = "{\"seq\":2,\"command\":\"launch\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_write_message(fds[1], m1, strlen(m1)));
    TEST_ASSERT_EQUAL_INT(0, dap_write_message(fds[1], m2, strlen(m2)));

    char   buf[256];
    size_t len = 0;

    /* First message reads back exactly, with no over-read into the second. */
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(fds[0], buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL_UINT(strlen(m1), (unsigned)len);
    TEST_ASSERT_EQUAL_STRING(m1, buf);

    /* Second message. */
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(fds[0], buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL_STRING(m2, buf);

    /* Closing the write end yields EOF (0). */
    close(fds[1]);
    TEST_ASSERT_EQUAL_INT(0, dap_read_message(fds[0], buf, sizeof buf, &len));
    close(fds[0]);
}

void test_dap_read_rejects_oversize_body(void)
{
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(fds));
    /* Advertise a body larger than the reader's buffer. */
    const char *hdr = "Content-Length: 100000\r\n\r\n";
    TEST_ASSERT_TRUE(write(fds[1], hdr, strlen(hdr)) == (ssize_t)strlen(hdr));
    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(-1, dap_read_message(fds[0], buf, sizeof buf, &len));
    close(fds[0]);
    close(fds[1]);
}

/* ── Lifecycle dispatch (over a socketpair; updi_fd = -1 skips UPDI) ───────── */

void test_dap_initialize_handshake(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req =
        "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
        "\"arguments\":{}}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char   buf[512];
    size_t len;
    /* response */
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"response\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"initialize\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"request_seq\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"success\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "supportsConfigurationDoneRequest"));
    /* followed by the initialized event */
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"event\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"event\":\"initialized\""));

    close(sp[0]); close(sp[1]);
}

void test_dap_configuration_done_emits_stopped_entry(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req = "{\"seq\":7,\"command\":\"configurationDone\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char   buf[512];
    size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"configurationDone\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"event\":\"stopped\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"entry\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"threadId\":1"));

    close(sp[0]); close(sp[1]);
}

void test_dap_disconnect_closes_session(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req = "{\"seq\":9,\"command\":\"disconnect\"}";
    /* dispatch returns 1 = the session should close */
    TEST_ASSERT_EQUAL_INT(1, dap_dispatch(&s, req, strlen(req)));

    char   buf[512];
    size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"disconnect\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"success\":true"));

    close(sp[0]); close(sp[1]);
}

void test_dap_unknown_request_returns_error(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req = "{\"seq\":5,\"command\":\"frobnicate\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char   buf[512];
    size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"frobnicate\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"success\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"message\""));

    close(sp[0]); close(sp[1]);
}

/* ── Phase 17: execution control + stop events ───────────────────────────── */

void test_dap_continue_responds_and_emits_continued(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req = "{\"seq\":11,\"command\":\"continue\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char buf[512]; size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"continue\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"allThreadsContinued\":true"));
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"event\":\"continued\""));

    close(sp[0]); close(sp[1]);
}

void test_dap_pause_emits_stopped_pause(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req = "{\"seq\":12,\"command\":\"pause\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char buf[512]; size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"pause\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"event\":\"stopped\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"pause\""));

    close(sp[0]); close(sp[1]);
}

void test_dap_step_emits_stopped_step(void)
{
    const char *cmds[] = { "next", "stepIn", "stepOut" };
    for (unsigned i = 0; i < 3; i++) {
        int sp[2];
        TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
        dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

        char req[64];
        snprintf(req, sizeof req, "{\"seq\":13,\"command\":\"%s\"}", cmds[i]);
        TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

        char buf[512]; size_t len;
        TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
        TEST_ASSERT_NOT_NULL(strstr(buf, cmds[i]));
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"success\":true"));
        TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"event\":\"stopped\""));
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"step\""));

        close(sp[0]); close(sp[1]);
    }
}

void test_dap_stack_trace_returns_one_frame(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req = "{\"seq\":14,\"command\":\"stackTrace\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char buf[1024]; size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"stackTrace\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stackFrames\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"totalFrames\":1"));

    close(sp[0]); close(sp[1]);
}

void test_dap_scopes_returns_empty(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };

    const char *req = "{\"seq\":15,\"command\":\"scopes\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char buf[512]; size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"scopes\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"scopes\":[]"));

    close(sp[0]); close(sp[1]);
}

/* ── Phase 18: source breakpoints ─────────────────────────────────────────── */

void test_dap_set_breakpoints_responds_with_per_line_entries(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };
    dap_bp_reset(&s);

    /* Two source breakpoints; no ELF/target in the unit test, so they resolve
     * unverified — but the per-line response shape (ids, lines) is exercised. */
    const char *req =
        "{\"seq\":20,\"command\":\"setBreakpoints\",\"arguments\":{"
        "\"source\":{\"path\":\"main.c\"},"
        "\"breakpoints\":[{\"line\":139},{\"line\":141,\"condition\":\"n==3\"}]}}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, req, strlen(req)));

    char buf[1024]; size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"setBreakpoints\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"breakpoints\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"line\":139"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"line\":141"));
    /* Two entries recorded in the session table; condition captured. */
    TEST_ASSERT_TRUE(s.bps[0].in_use && s.bps[1].in_use);
    TEST_ASSERT_EQUAL_INT(139, s.bps[0].line);
    TEST_ASSERT_EQUAL_STRING("n==3", s.bps[1].condition);
    TEST_ASSERT_EQUAL_INT(3, s.next_bp_id);

    close(sp[0]); close(sp[1]);
}

/* A second setBreakpoints for the same source replaces the prior set. */
void test_dap_set_breakpoints_replaces_prior_set_for_source(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    dap_session s = { sp[1], -1, NULL, NULL, NULL, false, 0 };
    dap_bp_reset(&s);

    const char *r1 =
        "{\"seq\":1,\"command\":\"setBreakpoints\",\"arguments\":{"
        "\"source\":{\"path\":\"main.c\"},\"breakpoints\":[{\"line\":10},{\"line\":20}]}}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, r1, strlen(r1)));
    char buf[1024]; size_t len;
    (void)dap_read_message(sp[0], buf, sizeof buf, &len);

    const char *r2 =
        "{\"seq\":2,\"command\":\"setBreakpoints\",\"arguments\":{"
        "\"source\":{\"path\":\"main.c\"},\"breakpoints\":[{\"line\":30}]}}";
    TEST_ASSERT_EQUAL_INT(0, dap_dispatch(&s, r2, strlen(r2)));
    (void)dap_read_message(sp[0], buf, sizeof buf, &len);

    /* Only the single line-30 breakpoint remains in the table. */
    int live = 0, line30 = 0;
    for (int i = 0; i < DAP_MAX_BREAKPOINTS; i++) {
        if (!s.bps[i].in_use) continue;
        live++;
        if (s.bps[i].line == 30) line30 = 1;
    }
    TEST_ASSERT_EQUAL_INT(1, live);
    TEST_ASSERT_TRUE(line30);

    close(sp[0]); close(sp[1]);
}

/* dap_serve(): accept (via the rsp_accept stub) one client whose request
 * stream is pre-loaded, run the select/read/dispatch loop, and return 0 when
 * the client disconnects. */
void test_dap_serve_runs_handshake_to_disconnect(void)
{
    int sp[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sp));

    /* Pre-load the client→server requests so select() sees them immediately. */
    const char *init = "{\"seq\":1,\"command\":\"initialize\"}";
    const char *disc = "{\"seq\":2,\"command\":\"disconnect\"}";
    TEST_ASSERT_EQUAL_INT(0, dap_write_message(sp[0], init, strlen(init)));
    TEST_ASSERT_EQUAL_INT(0, dap_write_message(sp[0], disc, strlen(disc)));

    g_accept_fd = sp[1];                /* dap_serve "accepts" this fd */
    volatile sig_atomic_t quit = 0;
    int rc = dap_serve(0 /*listen fd ignored by stub*/, -1 /*no UPDI*/,
                       NULL, NULL, NULL, &quit, false);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Drain the server→client side: initialize response, initialized event,
     * then the disconnect response. */
    char   buf[512];
    size_t len;
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"initialize\""));
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"event\":\"initialized\""));
    TEST_ASSERT_EQUAL_INT(1, dap_read_message(sp[0], buf, sizeof buf, &len));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"command\":\"disconnect\""));

    g_accept_fd = -1;
    close(sp[0]); close(sp[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dj_parse_extracts_request_fields);
    RUN_TEST(test_dj_parse_handles_arrays_and_nesting);
    RUN_TEST(test_dj_strcpy_decodes_escapes);
    RUN_TEST(test_dj_escape_quotes_backslash_and_controls);
    RUN_TEST(test_dj_parse_rejects_malformed);
    RUN_TEST(test_dj_parse_respects_token_cap);
    RUN_TEST(test_dap_framing_round_trip_over_pipe);
    RUN_TEST(test_dap_read_rejects_oversize_body);
    RUN_TEST(test_dap_initialize_handshake);
    RUN_TEST(test_dap_configuration_done_emits_stopped_entry);
    RUN_TEST(test_dap_disconnect_closes_session);
    RUN_TEST(test_dap_unknown_request_returns_error);
    RUN_TEST(test_dap_continue_responds_and_emits_continued);
    RUN_TEST(test_dap_pause_emits_stopped_pause);
    RUN_TEST(test_dap_step_emits_stopped_step);
    RUN_TEST(test_dap_stack_trace_returns_one_frame);
    RUN_TEST(test_dap_scopes_returns_empty);
    RUN_TEST(test_dap_set_breakpoints_responds_with_per_line_entries);
    RUN_TEST(test_dap_set_breakpoints_replaces_prior_set_for_source);
    RUN_TEST(test_dap_serve_runs_handshake_to_disconnect);
    return UNITY_END();
}
