/* tests/test_rsp.c — Unit tests for src/gdb_rsp.c
 *
 * Uses socketpair() as a stand-in for a real GDB TCP client.
 * All target-side calls (UPDI / FSM / monitor) are intercepted with
 * --wrap stubs and recorded in static structures inspected by tests.
 */
#include "unity/unity.h"
#include "gdb_rsp.h"
#include "elf_parser.h"
#include "fsm_mapper.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

extern int usleep(unsigned int);

/* ── Mock state ─────────────────────────────────────────────────────── */

#define MAX_MOCK_CALLS 64

typedef struct { uint32_t addr; size_t len; uint8_t data[64]; } MockMemOp;

static int      mock_run_calls;
static int      mock_step_calls;
static int      mock_invalidate_calls;
static int      mock_halt_calls;
static int      mock_halt_failures_left;
static int      mock_build_calls;

static MockMemOp mock_writes[MAX_MOCK_CALLS];
static int       mock_write_count;
static MockMemOp mock_flash_writes[MAX_MOCK_CALLS];
static int       mock_flash_write_count;
static MockMemOp mock_reads[MAX_MOCK_CALLS];
static int       mock_read_count;

static uint8_t   mock_read_canned[64];
static size_t   mock_read_canned_len;

static int       mock_monitor_rc;
static char      mock_monitor_last_body[256];
static int       mock_monitor_calls;

static int       mock_active_thread = 1;
static int       mock_get_regs_last_tid;

/* Fake FSM context populated by individual tests. */
static FsmContext fake_fsm;

/* ── --wrap stubs ───────────────────────────────────────────────────── */

int __wrap_updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)fd;
    if (mock_read_count < MAX_MOCK_CALLS) {
        mock_reads[mock_read_count].addr = addr;
        mock_reads[mock_read_count].len  = len;
        ++mock_read_count;
    }
    if (mock_read_canned_len) {
        size_t n = len < mock_read_canned_len ? len : mock_read_canned_len;
        memcpy(buf, mock_read_canned, n);
        if (n < len) memset(buf + n, 0, len - n);
    } else {
        memset(buf, 0xAB, len);
    }
    return 0;
}

int __wrap_updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len)
{
    (void)fd;
    if (mock_write_count < MAX_MOCK_CALLS) {
        mock_writes[mock_write_count].addr = addr;
        mock_writes[mock_write_count].len  = len;
        size_t n = len < sizeof mock_writes[0].data ? len : sizeof mock_writes[0].data;
        memcpy(mock_writes[mock_write_count].data, buf, n);
        ++mock_write_count;
    }
    return 0;
}

int __wrap_updi_nvm_write_flash(int fd, uint32_t addr, const uint8_t *buf, size_t len)
{
    (void)fd;
    if (mock_flash_write_count < MAX_MOCK_CALLS) {
        mock_flash_writes[mock_flash_write_count].addr = addr;
        mock_flash_writes[mock_flash_write_count].len  = len;
        size_t n = len < sizeof mock_flash_writes[0].data ? len : sizeof mock_flash_writes[0].data;
        memcpy(mock_flash_writes[mock_flash_write_count].data, buf, n);
        ++mock_flash_write_count;
    }
    return 0;
}

int __wrap_updi_halt(int fd)
{
    (void)fd;
    ++mock_halt_calls;
    if (mock_halt_failures_left > 0) { --mock_halt_failures_left; return -1; }
    return 0;
}

int __wrap_updi_run(int fd)
{
    (void)fd;
    ++mock_run_calls;
    return 0;
}

int __wrap_updi_step(int fd)
{
    (void)fd;
    ++mock_step_calls;
    return 0;
}

int __wrap_updi_console_poll(int updi_fd, int rsp_fd)
{
    (void)updi_fd; (void)rsp_fd;
    return 0;
}

int __wrap_fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int updi_fd)
{
    (void)idx; (void)updi_fd;
    ++mock_build_calls;
    if (ctx != NULL) ctx->valid = true;
    return ctx ? ctx->thread_count : 0;
}

int __wrap_fsm_get_registers(const FsmContext *ctx, int tid, char *reg_buf)
{
    (void)ctx;
    mock_get_regs_last_tid = tid;
    /* Produce 78 hex chars: zeroed bytes 0..34, then PC in bytes 35..38. */
    memset(reg_buf, '0', 78);
    reg_buf[78] = '\0';
    /* Embed PC = 0xDEADBEEF (little-endian) at hex positions 70-77. */
    static const char pc_hex[] = "efbeadde";
    memcpy(reg_buf + 70, pc_hex, 8);
    return 0;
}

int __wrap_fsm_get_active_thread(const FsmContext *ctx)
{
    (void)ctx;
    return mock_active_thread;
}

void __wrap_fsm_invalidate(FsmContext *ctx)
{
    (void)ctx;
    ++mock_invalidate_calls;
}

int __wrap_monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx,
                            const char *hex_body)
{
    (void)rsp_fd; (void)updi_fd; (void)idx;
    ++mock_monitor_calls;
    strncpy(mock_monitor_last_body, hex_body, sizeof mock_monitor_last_body - 1u);
    mock_monitor_last_body[sizeof mock_monitor_last_body - 1u] = '\0';
    return mock_monitor_rc;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static int sock_pair[2];

static void make_pair(void)
{
    TEST_ASSERT_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sock_pair));
}

static void close_pair(void)
{
    if (sock_pair[0] >= 0) close(sock_pair[0]);
    if (sock_pair[1] >= 0) close(sock_pair[1]);
}

static uint8_t xor_csum(const char *s, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) c ^= (uint8_t)s[i];
    return c;
}

/* Drain everything currently readable from fd into buf (non-blocking). */
static size_t drain(int fd, char *buf, size_t cap)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    size_t off = 0;
    for (;;) {
        ssize_t n = read(fd, buf + off, cap - off - 1u);
        if (n <= 0) break;
        off += (size_t)n;
        if (off >= cap - 1u) break;
    }
    buf[off] = '\0';
    fcntl(fd, F_SETFL, flags);
    return off;
}

/* Extract the most recent $payload#XX packet from a stream that may contain
 * stray + / - ack bytes. Returns 0 on success and fills payload[]. */
static int last_packet_payload(const char *stream, char *payload, size_t cap)
{
    const char *last_dollar = NULL;
    for (const char *p = stream; *p; ++p) if (*p == '$') last_dollar = p;
    if (last_dollar == NULL) return -1;
    const char *hash = strchr(last_dollar, '#');
    if (hash == NULL) return -1;
    size_t plen = (size_t)(hash - last_dollar - 1);
    if (plen + 1u > cap) return -1;
    memcpy(payload, last_dollar + 1, plen);
    payload[plen] = '\0';
    return 0;
}

static void reset_mocks(void)
{
    mock_run_calls = mock_step_calls = mock_invalidate_calls = 0;
    mock_halt_calls = mock_halt_failures_left = mock_build_calls = 0;
    mock_write_count = mock_flash_write_count = mock_read_count = 0;
    memset(mock_writes, 0, sizeof mock_writes);
    memset(mock_flash_writes, 0, sizeof mock_flash_writes);
    memset(mock_reads, 0, sizeof mock_reads);
    memset(mock_read_canned, 0, sizeof mock_read_canned);
    mock_read_canned_len = 0;
    mock_monitor_rc = 0;
    mock_monitor_calls = 0;
    memset(mock_monitor_last_body, 0, sizeof mock_monitor_last_body);
    mock_active_thread = 1;
    mock_get_regs_last_tid = 0;
    memset(&fake_fsm, 0, sizeof fake_fsm);
    fake_fsm.valid = true;
    fake_fsm.thread_count = 1;
    fake_fsm.threads[0].gdb_id = 1;
    strcpy(fake_fsm.threads[0].name, "FSM_A");
    fake_fsm.threads[0].is_active = true;
    rsp_set_noack(false);
}

/* Build a default context + handlers attached to sock_pair[1] (server side). */
static int g_tid_var, c_tid_var;
static volatile sig_atomic_t g_quit_var;
static int g_gdb_fd_var;

static void build_ctx(RspContext *ctx, RspHandlers *h)
{
    g_tid_var = 0; c_tid_var = 0; g_quit_var = 0;
    g_gdb_fd_var = sock_pair[1];
    memset(ctx, 0, sizeof *ctx);
    ctx->updi_fd    = 99;
    ctx->fsm        = &fake_fsm;
    ctx->idx        = NULL;
    ctx->g_thread_p = &g_tid_var;
    ctx->c_thread_p = &c_tid_var;
    ctx->gdb_fd_p   = &g_gdb_fd_var;
    ctx->quit_p     = &g_quit_var;
    rsp_default_handlers(h, ctx);
}

void setUp(void)    { make_pair(); reset_mocks(); }
void tearDown(void) { close_pair(); }

/* ── LLR-RSP-01: socket options ─────────────────────────────────────── */

static void rsp_listen_sets_so_reuseaddr_before_bind(void)
{
    int lfd = rsp_listen(0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, lfd);
    int v = 0; socklen_t sl = sizeof v;
    TEST_ASSERT_EQUAL(0, getsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &v, &sl));
    TEST_ASSERT_NOT_EQUAL(0, v);
    close(lfd);
}

static void rsp_accept_sets_tcp_nodelay_on_client_socket(void)
{
    int lfd = rsp_listen(0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, lfd);
    struct sockaddr_in addr; socklen_t alen = sizeof addr;
    TEST_ASSERT_EQUAL(0, getsockname(lfd, (struct sockaddr *)&addr, &alen));

    pid_t pid = fork();
    if (pid == 0) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        connect(s, (struct sockaddr *)&addr, alen);
        usleep(50000);
        close(s);
        _exit(0);
    }
    int cfd = rsp_accept(lfd);
    TEST_ASSERT_GREATER_OR_EQUAL(0, cfd);
    int v = 0; socklen_t sl = sizeof v;
    TEST_ASSERT_EQUAL(0, getsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &v, &sl));
    TEST_ASSERT_NOT_EQUAL(0, v);
    close(cfd); close(lfd);
    int st; waitpid(pid, &st, 0);
}

/* ── LLR-RSP-02: rsp_recv_packet ────────────────────────────────────── */

static void send_frame(int fd, const char *payload)
{
    uint8_t c = xor_csum(payload, strlen(payload));
    char buf[2100];
    int n = snprintf(buf, sizeof buf, "$%s#%02x", payload, c);
    write(fd, buf, (size_t)n);
}

static void rsp_recv_packet_discards_leading_ack_nak_bytes(void)
{
    /* Write +/- noise then a clean packet */
    write(sock_pair[0], "+-+", 3);
    send_frame(sock_pair[0], "g");
    char buf[64];
    int rc = rsp_recv_packet(sock_pair[1], buf, sizeof buf);
    TEST_ASSERT_EQUAL(1, rc);
    TEST_ASSERT_EQUAL_STRING("g", buf);
}

static void rsp_recv_packet_sends_plus_on_valid_checksum(void)
{
    send_frame(sock_pair[0], "qSupported");
    char buf[64];
    int rc = rsp_recv_packet(sock_pair[1], buf, sizeof buf);
    TEST_ASSERT_EQUAL((int)strlen("qSupported"), rc);
    char ack;
    TEST_ASSERT_EQUAL(1, read(sock_pair[0], &ack, 1));
    TEST_ASSERT_EQUAL('+', ack);
}

static void rsp_recv_packet_sends_minus_and_returns_minus1_on_bad_checksum(void)
{
    /* manually frame with a wrong checksum */
    const char *body = "g";
    char buf[16];
    snprintf(buf, sizeof buf, "$%s#%02x", body, 0xFF);
    write(sock_pair[0], buf, strlen(buf));
    char pbuf[64];
    int rc = rsp_recv_packet(sock_pair[1], pbuf, sizeof pbuf);
    TEST_ASSERT_EQUAL(-1, rc);
    char ack;
    TEST_ASSERT_EQUAL(1, read(sock_pair[0], &ack, 1));
    TEST_ASSERT_EQUAL('-', ack);
}

/* ── LLR-RSP-03: `g` ─────────────────────────────────────────────────── */

static void on_read_regs_g_returns_78_char_hex_string(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "g", &h);
    char stream[256]; drain(sock_pair[0], stream, sizeof stream);
    char payload[256];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(78u, strlen(payload));
}

static void on_read_regs_g_places_pc_little_endian_at_positions_70_77(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "g", &h);
    char stream[256]; drain(sock_pair[0], stream, sizeof stream);
    char payload[256];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    /* Mock writes PC=0xDEADBEEF little-endian => "efbeadde" at 70..77 */
    TEST_ASSERT_EQUAL_STRING_LEN("efbeadde", payload + 70, 8);
}

/* ── LLR-RSP-04: `G` and `P` ────────────────────────────────────────── */

static void on_write_regs_G_writes_all_registers_via_updi_mem_write(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* G followed by 78 hex chars (39 bytes worth of register data). */
    char pkt[128] = "G";
    for (int i = 0; i < 78; ++i) pkt[1 + i] = (i & 1) ? '0' : '1';
    pkt[1 + 78] = '\0';
    rsp_dispatch(sock_pair[1], pkt, &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_GREATER_THAN(0, mock_write_count);
}

static void on_write_regs_P_writes_single_register_via_updi_mem_write(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "P5=ab", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_write_count);
    TEST_ASSERT_EQUAL(0x1000u + 5u, mock_writes[0].addr);
    TEST_ASSERT_EQUAL(0xAB, mock_writes[0].data[0]);
}

/* ── LLR-RSP-05: `m` ─────────────────────────────────────────────────── */

static void on_read_mem_m_calls_updi_mem_read_and_returns_hex(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_read_canned_len = 4;
    mock_read_canned[0] = 0xDE; mock_read_canned[1] = 0xAD;
    mock_read_canned[2] = 0xBE; mock_read_canned[3] = 0xEF;
    rsp_dispatch(sock_pair[1], "m100,4", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("deadbeef", payload);
    TEST_ASSERT_EQUAL(1, mock_read_count);
    TEST_ASSERT_EQUAL(0x100u, mock_reads[0].addr);
    TEST_ASSERT_EQUAL(4u, mock_reads[0].len);
}

/* ── LLR-RSP-06: `M` / `X` ──────────────────────────────────────────── */

static void on_write_mem_M_calls_updi_mem_write_for_sram_address(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "M800100,4:deadbeef", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_write_count);
    TEST_ASSERT_EQUAL(0x100u, mock_writes[0].addr);
}

static void on_write_mem_X_calls_nvm_write_flash_for_flash_address(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "X100,2:\x98\x95", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_flash_write_count);
}

/* ── LLR-RSP-07: insert bp ──────────────────────────────────────────── */

static void on_insert_bp_writes_break_opcode_and_saves_original_word(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_read_canned_len = 2;
    mock_read_canned[0] = 0x12; mock_read_canned[1] = 0x34;
    rsp_dispatch(sock_pair[1], "Z0,200,2", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_read_count);
    TEST_ASSERT_EQUAL(1, mock_flash_write_count);
    /* Wrote BREAK little-endian: 0x98, 0x95 */
    TEST_ASSERT_EQUAL(0x98, mock_flash_writes[0].data[0]);
    TEST_ASSERT_EQUAL(0x95, mock_flash_writes[0].data[1]);
}

static void on_insert_bp_duplicate_returns_ok_without_reflash(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "Z0,201,2", &h);
    rsp_dispatch(sock_pair[1], "Z0,201,2", &h);
    char stream[128]; drain(sock_pair[0], stream, sizeof stream);
    /* Both replies should be OK; only one flash write. */
    TEST_ASSERT_EQUAL(1, mock_flash_write_count);
}

/* ── LLR-RSP-08: remove bp ──────────────────────────────────────────── */

static void on_remove_bp_restores_saved_instruction_word(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_read_canned_len = 2;
    mock_read_canned[0] = 0xAA; mock_read_canned[1] = 0xBB;
    rsp_dispatch(sock_pair[1], "Z0,300,2", &h);
    int flash_after_insert = mock_flash_write_count;
    rsp_dispatch(sock_pair[1], "z0,300,2", &h);
    TEST_ASSERT_EQUAL(flash_after_insert + 1, mock_flash_write_count);
    TEST_ASSERT_EQUAL(0xAA, mock_flash_writes[mock_flash_write_count - 1].data[0]);
    TEST_ASSERT_EQUAL(0xBB, mock_flash_writes[mock_flash_write_count - 1].data[1]);
}

static void on_remove_bp_unknown_address_returns_error_reply(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "z0,deadbeef,2", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL('E', payload[0]);
}

/* ── LLR-RSP-09: bp table overflow ─────────────────────────────────── */

static void on_insert_bp_returns_E08_when_table_full(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    char pkt[32];
    for (int i = 0; i < RSP_MAX_BREAKPOINTS; ++i) {
        snprintf(pkt, sizeof pkt, "Z0,%x,2", 0x1000 + i);
        rsp_dispatch(sock_pair[1], pkt, &h);
    }
    char drain_buf[2048]; drain(sock_pair[0], drain_buf, sizeof drain_buf);
    snprintf(pkt, sizeof pkt, "Z0,%x,2", 0x2000);
    rsp_dispatch(sock_pair[1], pkt, &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("E08", payload);
}

/* ── LLR-RSP-10: step ───────────────────────────────────────────────── */

static void on_step_s_calls_updi_step_and_sends_T05_stop_reason(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "s", &h);
    TEST_ASSERT_EQUAL(1, mock_step_calls);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T05thread:", 10));
}

/* ── LLR-RSP-11: continue ───────────────────────────────────────────── */

static void on_continue_calls_updi_run_then_fsm_invalidate(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "c", &h);
    TEST_ASSERT_EQUAL(1, mock_run_calls);
    TEST_ASSERT_EQUAL(1, mock_invalidate_calls);
}

static void on_continue_rebuilds_thread_list_after_halt_and_sends_stop(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_halt_failures_left = 3;
    rsp_dispatch(sock_pair[1], "c", &h);
    TEST_ASSERT_GREATER_OR_EQUAL(4, mock_halt_calls);  /* 3 failures + 1 success */
    TEST_ASSERT_EQUAL(1, mock_build_calls);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T05", 3));
}

/* ── LLR-RSP-12: qSupported / qAttached inline ─────────────────────── */

static void rsp_dispatch_qsupported_returns_feature_string_no_target_access(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qSupported:multiprocess+", &h);
    char stream[256]; drain(sock_pair[0], stream, sizeof stream);
    char payload[256];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_NOT_NULL(strstr(payload, "PacketSize"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "QStartNoAckMode+"));
    TEST_ASSERT_EQUAL(0, mock_read_count);
    TEST_ASSERT_EQUAL(0, mock_write_count);
}

static void rsp_dispatch_qattached_returns_1_no_target_access(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qAttached", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("1", payload);
    TEST_ASSERT_EQUAL(0, mock_read_count);
}

/* ── LLR-RSP-13: D ──────────────────────────────────────────────────── */

static void on_detach_D_resumes_target_closes_socket_resets_gdb_fd(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "D", &h);
    TEST_ASSERT_EQUAL(1, mock_run_calls);
    TEST_ASSERT_EQUAL(-1, g_gdb_fd_var);
    /* socket should be closed; mark to avoid double-close in teardown */
    sock_pair[1] = -1;
}

/* ── LLR-RSP-14: k ──────────────────────────────────────────────────── */

static void on_kill_k_sets_g_quit_to_1(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "k", &h);
    TEST_ASSERT_EQUAL(1, g_quit_var);
}

/* ── LLR-RSP-15: monitor ────────────────────────────────────────────── */

static void on_monitor_qRcmd_passes_hex_body_to_monitor_dispatch(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qRcmd,6176726f73206576656e7473", &h);
    TEST_ASSERT_EQUAL(1, mock_monitor_calls);
    TEST_ASSERT_EQUAL_STRING("6176726f73206576656e7473", mock_monitor_last_body);
}

static void on_monitor_returns_ok_when_monitor_dispatch_succeeds(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_monitor_rc = 0;
    rsp_dispatch(sock_pair[1], "qRcmd,ab", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
}

static void on_monitor_sends_o_packet_error_on_updi_failure(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_monitor_rc = -1;
    rsp_dispatch(sock_pair[1], "qRcmd,ab", &h);
    char stream[256]; drain(sock_pair[0], stream, sizeof stream);
    /* Expect first packet starts with 'O' (O-packet). */
    const char *first = strchr(stream, '$');
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL('O', first[1]);
}

/* ── LLR-RSP-16: H<g|c><tid> ────────────────────────────────────────── */

static void H_packet_stores_thread_id_for_register_operations(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* Build a richer FSM with thread 3 */
    fake_fsm.thread_count = 3;
    fake_fsm.threads[1].gdb_id = 2; strcpy(fake_fsm.threads[1].name, "B");
    fake_fsm.threads[2].gdb_id = 3; strcpy(fake_fsm.threads[2].name, "C");
    rsp_dispatch(sock_pair[1], "Hg3", &h);
    TEST_ASSERT_EQUAL(3, g_tid_var);
    rsp_dispatch(sock_pair[1], "g", &h);
    TEST_ASSERT_EQUAL(3, mock_get_regs_last_tid);
}

static void H_packet_minus1_and_0_both_map_to_active_fsm_thread(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_active_thread = 7;
    rsp_dispatch(sock_pair[1], "Hg-1", &h);
    TEST_ASSERT_EQUAL(7, g_tid_var);
    rsp_dispatch(sock_pair[1], "Hg0", &h);
    TEST_ASSERT_EQUAL(7, g_tid_var);
}

/* ── Runner ─────────────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(rsp_listen_sets_so_reuseaddr_before_bind);
    RUN_TEST(rsp_accept_sets_tcp_nodelay_on_client_socket);
    RUN_TEST(rsp_recv_packet_discards_leading_ack_nak_bytes);
    RUN_TEST(rsp_recv_packet_sends_plus_on_valid_checksum);
    RUN_TEST(rsp_recv_packet_sends_minus_and_returns_minus1_on_bad_checksum);
    RUN_TEST(on_read_regs_g_returns_78_char_hex_string);
    RUN_TEST(on_read_regs_g_places_pc_little_endian_at_positions_70_77);
    RUN_TEST(on_write_regs_G_writes_all_registers_via_updi_mem_write);
    RUN_TEST(on_write_regs_P_writes_single_register_via_updi_mem_write);
    RUN_TEST(on_read_mem_m_calls_updi_mem_read_and_returns_hex);
    RUN_TEST(on_write_mem_M_calls_updi_mem_write_for_sram_address);
    RUN_TEST(on_write_mem_X_calls_nvm_write_flash_for_flash_address);
    RUN_TEST(on_insert_bp_writes_break_opcode_and_saves_original_word);
    RUN_TEST(on_insert_bp_duplicate_returns_ok_without_reflash);
    RUN_TEST(on_remove_bp_restores_saved_instruction_word);
    RUN_TEST(on_remove_bp_unknown_address_returns_error_reply);
    RUN_TEST(on_insert_bp_returns_E08_when_table_full);
    RUN_TEST(on_step_s_calls_updi_step_and_sends_T05_stop_reason);
    RUN_TEST(on_continue_calls_updi_run_then_fsm_invalidate);
    RUN_TEST(on_continue_rebuilds_thread_list_after_halt_and_sends_stop);
    RUN_TEST(rsp_dispatch_qsupported_returns_feature_string_no_target_access);
    RUN_TEST(rsp_dispatch_qattached_returns_1_no_target_access);
    RUN_TEST(on_detach_D_resumes_target_closes_socket_resets_gdb_fd);
    RUN_TEST(on_kill_k_sets_g_quit_to_1);
    RUN_TEST(on_monitor_qRcmd_passes_hex_body_to_monitor_dispatch);
    RUN_TEST(on_monitor_returns_ok_when_monitor_dispatch_succeeds);
    RUN_TEST(on_monitor_sends_o_packet_error_on_updi_failure);
    RUN_TEST(H_packet_stores_thread_id_for_register_operations);
    RUN_TEST(H_packet_minus1_and_0_both_map_to_active_fsm_thread);
    return UNITY_END();
}
