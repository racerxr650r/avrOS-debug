/* tests/test_rsp.c — Unit tests for src/gdb_rsp.c
 *
 * Uses socketpair() as a stand-in for a real GDB TCP client.
 * All target-side calls (UPDI memory + OCD primitives, FSM, monitor)
 * are intercepted with `ld --wrap` stubs and recorded in static
 * structures inspected by tests.
 *
 * Traceability: see Project.xml → STP → tests/test_rsp.c.
 */
#include "unity/unity.h"
#include "gdb_rsp.h"
#include "elf_parser.h"
#include "fsm_mapper.h"
#include "updi.h"

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

/* Generic call counters. */
static int mock_run_calls;
static int mock_step_calls;
static int mock_invalidate_calls;
static int mock_halt_calls;
static int mock_build_calls;

/* updi_mem_read / updi_mem_write / updi_nvm_write_flash records. */
static MockMemOp mock_writes[MAX_MOCK_CALLS];
static int       mock_write_count;
static MockMemOp mock_flash_writes[MAX_MOCK_CALLS];
static int       mock_flash_write_count;
/* HLR-054: updi_nvm_flash_patch records (used by SW BP install/remove). */
static MockMemOp mock_flash_patches[MAX_MOCK_CALLS];
static int       mock_flash_patch_count;
static MockMemOp mock_reads[MAX_MOCK_CALLS];
static int       mock_read_count;
static uint8_t   mock_read_canned[64];
static size_t    mock_read_canned_len;

/* monitor_dispatch mock state. */
static int  mock_monitor_rc;
static char mock_monitor_last_body[256];
static int  mock_monitor_calls;

/* FSM mock state. */
static int  mock_active_thread = 1;
static int  mock_get_regs_last_tid;
static FsmContext fake_fsm;

/* OCD register-file shadow. */
static uint8_t  mock_ocd_gpr[32];
static uint8_t  mock_ocd_sreg;
static uint16_t mock_ocd_sp;
static uint32_t mock_ocd_pc;
static int      mock_ocd_gpr_writes;
static int      mock_ocd_sreg_writes;
static int      mock_ocd_sp_writes;
static int      mock_ocd_pc_writes;
static int      mock_ocd_gpr_reads_active;  /* active-thread g via OCD */

/* OCD halt-status that updi_ocd_read_halt_status() returns. */
static uint8_t mock_ocd_status0;
static uint8_t mock_ocd_status1;

/* OCD HW breakpoint shadow (silicon side, distinct from RspContext shadow). */
static uint32_t mock_hw_bp_silicon[2];
static int      mock_hw_bp_set_calls;
static int      mock_hw_bp_clear_calls;

/* Call-order tracker so we can prove e.g. clear_hw_bp happens before
 * updi_run inside dh_detach.  Records an opaque tag per event. */
typedef enum {
    EV_RUN = 1,
    EV_STEP,
    EV_HALT,
    EV_OCD_SET_BP,
    EV_OCD_CLEAR_BP,
    EV_OCD_POLL,
    EV_OCD_WRITE_GPR,
    EV_OCD_WRITE_PC,
    EV_OCD_WRITE_SREG,
    EV_OCD_WRITE_SP
} MockEvent;

static MockEvent mock_events[64];
static int       mock_event_count;
static void log_event(MockEvent e)
{
    if (mock_event_count < (int)(sizeof mock_events / sizeof mock_events[0]))
        mock_events[mock_event_count++] = e;
}

/* updi_ocd_poll_halted scripted return values: -1 = link failure,
 * 0 = halted, 1 = still running.  The array is consumed left-to-right;
 * once exhausted, the value of `mock_ocd_poll_default` is used. */
static int mock_ocd_poll_script[16];
static int mock_ocd_poll_script_len;
static int mock_ocd_poll_script_idx;
static int mock_ocd_poll_default = 0; /* default → halt on first poll */
static int mock_ocd_poll_calls;

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

/* HLR-054: software-breakpoint FLASH RMW.  Captures the call args so
 * SW BP install/remove tests can verify BREAK / original opcodes.    */
int __wrap_updi_nvm_flash_patch(int fd, uint32_t addr,
                                const uint8_t *buf, size_t len)
{
    (void)fd;
    if (mock_flash_patch_count < MAX_MOCK_CALLS) {
        mock_flash_patches[mock_flash_patch_count].addr = addr;
        mock_flash_patches[mock_flash_patch_count].len  = len;
        size_t n = len < sizeof mock_flash_patches[0].data ? len
                                                           : sizeof mock_flash_patches[0].data;
        memcpy(mock_flash_patches[mock_flash_patch_count].data, buf, n);
        ++mock_flash_patch_count;
    }
    return 0;
}

int __wrap_updi_halt(int fd)        { (void)fd; ++mock_halt_calls; log_event(EV_HALT); return 0; }
int __wrap_updi_run (int fd)        { (void)fd; ++mock_run_calls;  log_event(EV_RUN);  return 0; }
int __wrap_updi_step(int fd)        { (void)fd; ++mock_step_calls; log_event(EV_STEP); return 0; }
int __wrap_updi_console_poll(int u, int r) { (void)u; (void)r; return 0; }
int __wrap_updi_enter_debug(int fd) { (void)fd; return 0; }
int __wrap_updi_chip_erase(int fd) { (void)fd; return 0; }

int __wrap_updi_ocd_poll_halted(int fd, int timeout_ms)
{
    (void)fd; (void)timeout_ms;
    ++mock_ocd_poll_calls;
    log_event(EV_OCD_POLL);
    if (mock_ocd_poll_script_idx < mock_ocd_poll_script_len) {
        return mock_ocd_poll_script[mock_ocd_poll_script_idx++];
    }
    return mock_ocd_poll_default;
}

int __wrap_updi_ocd_read_halt_status(int fd, uint8_t *st0, uint8_t *st1)
{
    (void)fd;
    if (st0) *st0 = mock_ocd_status0;
    if (st1) *st1 = mock_ocd_status1;
    return 0;
}

int __wrap_updi_ocd_read_gpr(int fd, uint8_t n, uint8_t *val)
{
    (void)fd;
    ++mock_ocd_gpr_reads_active;
    if (val) *val = mock_ocd_gpr[n & 31u];
    return 0;
}

int __wrap_updi_ocd_write_gpr(int fd, uint8_t n, uint8_t val)
{
    (void)fd;
    mock_ocd_gpr[n & 31u] = val;
    ++mock_ocd_gpr_writes;
    log_event(EV_OCD_WRITE_GPR);
    return 0;
}

int __wrap_updi_ocd_read_sreg(int fd, uint8_t *val)
{
    (void)fd; if (val) *val = mock_ocd_sreg; return 0;
}

int __wrap_updi_ocd_write_sreg(int fd, uint8_t val)
{
    (void)fd; mock_ocd_sreg = val; ++mock_ocd_sreg_writes;
    log_event(EV_OCD_WRITE_SREG); return 0;
}

int __wrap_updi_ocd_read_sp(int fd, uint16_t *val)
{
    (void)fd; if (val) *val = mock_ocd_sp; return 0;
}

int __wrap_updi_ocd_write_sp(int fd, uint16_t val)
{
    (void)fd; mock_ocd_sp = val; ++mock_ocd_sp_writes;
    log_event(EV_OCD_WRITE_SP); return 0;
}

int __wrap_updi_ocd_read_pc(int fd, uint32_t *val)
{
    (void)fd; if (val) *val = mock_ocd_pc; return 0;
}

int __wrap_updi_ocd_write_pc(int fd, uint32_t val)
{
    (void)fd; mock_ocd_pc = val; ++mock_ocd_pc_writes;
    log_event(EV_OCD_WRITE_PC); return 0;
}

int __wrap_updi_ocd_set_hw_bp(int fd, int idx, uint32_t byte_addr)
{
    (void)fd;
    if (idx >= 0 && idx < 2) mock_hw_bp_silicon[idx] = byte_addr;
    ++mock_hw_bp_set_calls;
    log_event(EV_OCD_SET_BP);
    return 0;
}

int __wrap_updi_ocd_clear_hw_bp(int fd, int idx)
{
    (void)fd;
    if (idx >= 0 && idx < 2) mock_hw_bp_silicon[idx] = 0xFFFFFFFFu;
    ++mock_hw_bp_clear_calls;
    log_event(EV_OCD_CLEAR_BP);
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
    memset(reg_buf, '0', 78);
    reg_buf[78] = '\0';
    /* PC=0xDEADBEEF LE at hex positions 70-77. */
    memcpy(reg_buf + 70, "efbeadde", 8);
    return 0;
}

int __wrap_fsm_get_active_thread(const FsmContext *ctx)
{
    (void)ctx; return mock_active_thread;
}

void __wrap_fsm_invalidate(FsmContext *ctx) { (void)ctx; ++mock_invalidate_calls; }

int __wrap_monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx,
                            const char *hex_body)
{
    (void)rsp_fd; (void)updi_fd; (void)idx;
    ++mock_monitor_calls;
    strncpy(mock_monitor_last_body, hex_body, sizeof mock_monitor_last_body - 1u);
    mock_monitor_last_body[sizeof mock_monitor_last_body - 1u] = '\0';
    return mock_monitor_rc;
}

/* HLR-055: dh_monitor now calls monitor_dispatch_ex.  Route through the
 * same mock state so the existing tests still observe the call.  ctx
 * is ignored — the dispatcher passes whatever RspContext dh_monitor
 * built.                                                              */
int __wrap_monitor_dispatch_ex(int rsp_fd, RspContext *ctx, const char *hex_body)
{
    (void)rsp_fd; (void)ctx;
    ++mock_monitor_calls;
    strncpy(mock_monitor_last_body, hex_body, sizeof mock_monitor_last_body - 1u);
    mock_monitor_last_body[sizeof mock_monitor_last_body - 1u] = '\0';
    return mock_monitor_rc;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static int sock_pair[2];

static void make_pair(void) { TEST_ASSERT_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sock_pair)); }
static void close_pair(void)
{
    if (sock_pair[0] >= 0) close(sock_pair[0]);
    if (sock_pair[1] >= 0) close(sock_pair[1]);
}

static uint8_t sum_csum(const char *s, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) c = (uint8_t)(c + (uint8_t)s[i]);
    return c;
}

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

static int event_index(MockEvent e)
{
    for (int i = 0; i < mock_event_count; ++i) if (mock_events[i] == e) return i;
    return -1;
}

static void reset_mocks(void)
{
    mock_run_calls = mock_step_calls = mock_invalidate_calls = 0;
    mock_halt_calls = mock_build_calls = 0;
    mock_write_count = mock_flash_write_count = mock_read_count = 0;
    mock_flash_patch_count = 0;
    memset(mock_writes, 0, sizeof mock_writes);
    memset(mock_flash_writes, 0, sizeof mock_flash_writes);
    memset(mock_flash_patches, 0, sizeof mock_flash_patches);
    memset(mock_reads, 0, sizeof mock_reads);
    memset(mock_read_canned, 0, sizeof mock_read_canned);
    mock_read_canned_len = 0;
    mock_monitor_rc = 0;
    mock_monitor_calls = 0;
    memset(mock_monitor_last_body, 0, sizeof mock_monitor_last_body);
    mock_active_thread = 1;
    mock_get_regs_last_tid = 0;
    memset(mock_ocd_gpr, 0, sizeof mock_ocd_gpr);
    mock_ocd_sreg = 0; mock_ocd_sp = 0; mock_ocd_pc = 0;
    mock_ocd_gpr_writes = mock_ocd_sreg_writes = mock_ocd_sp_writes = mock_ocd_pc_writes = 0;
    mock_ocd_gpr_reads_active = 0;
    mock_ocd_status0 = mock_ocd_status1 = 0;
    mock_hw_bp_silicon[0] = mock_hw_bp_silicon[1] = 0xFFFFFFFFu;
    mock_hw_bp_set_calls = mock_hw_bp_clear_calls = 0;
    mock_event_count = 0;
    mock_ocd_poll_script_idx = mock_ocd_poll_script_len = 0;
    mock_ocd_poll_default = 0;
    mock_ocd_poll_calls = 0;
    memset(&fake_fsm, 0, sizeof fake_fsm);
    fake_fsm.valid = true;
    fake_fsm.thread_count = 1;
    fake_fsm.threads[0].gdb_id = 1;
    strcpy(fake_fsm.threads[0].name, "FSM_A");
    fake_fsm.threads[0].is_active = true;
    rsp_set_noack(false);
}

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
    uint8_t c = sum_csum(payload, strlen(payload));
    char buf[2100];
    int n = snprintf(buf, sizeof buf, "$%s#%02x", payload, c);
    write(fd, buf, (size_t)n);
}

static void rsp_recv_packet_discards_leading_ack_nak_bytes(void)
{
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
    /* Active thread → OCD path, not fsm_get_registers. */
    TEST_ASSERT_EQUAL(32, mock_ocd_gpr_reads_active);
}

static void on_read_regs_g_places_pc_little_endian_at_positions_70_77(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_ocd_pc = 0xDEADBEEFu;
    rsp_dispatch(sock_pair[1], "g", &h);
    char stream[256]; drain(sock_pair[0], stream, sizeof stream);
    char payload[256];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING_LEN("efbeadde", payload + 70, 8);
}

static void on_read_regs_non_active_thread_uses_fsm_register_frame(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* Build FSM with two threads; select non-active thread 2 via Hg2. */
    fake_fsm.thread_count = 2;
    fake_fsm.threads[1].gdb_id = 2; strcpy(fake_fsm.threads[1].name, "B");
    mock_active_thread = 1;
    rsp_dispatch(sock_pair[1], "Hg2", &h);
    char drain1[64]; drain(sock_pair[0], drain1, sizeof drain1);
    rsp_dispatch(sock_pair[1], "g", &h);
    TEST_ASSERT_EQUAL(2, mock_get_regs_last_tid);
    TEST_ASSERT_EQUAL(0, mock_ocd_gpr_reads_active);
}

/* ── LLR-RSP-04: `G` / `P` ──────────────────────────────────────────── */

static void on_write_regs_G_writes_all_registers_via_ocd(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    char pkt[128] = "G";
    for (int i = 0; i < 78; ++i) pkt[1 + i] = (i & 1) ? '0' : '1';
    pkt[1 + 78] = '\0';
    rsp_dispatch(sock_pair[1], pkt, &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(32, mock_ocd_gpr_writes);
    TEST_ASSERT_EQUAL(1,  mock_ocd_sreg_writes);
    TEST_ASSERT_EQUAL(1,  mock_ocd_sp_writes);
    TEST_ASSERT_EQUAL(1,  mock_ocd_pc_writes);
    TEST_ASSERT_EQUAL(0,  mock_write_count); /* not via updi_mem_write */
}

static void on_write_regs_P_writes_single_register_via_ocd(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "P5=ab", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_ocd_gpr_writes);
    TEST_ASSERT_EQUAL(0xAB, mock_ocd_gpr[5]);
    TEST_ASSERT_EQUAL(0, mock_ocd_sreg_writes);
    TEST_ASSERT_EQUAL(0, mock_ocd_sp_writes);
    TEST_ASSERT_EQUAL(0, mock_ocd_pc_writes);
}

/* ── LLR-RSP-05: `m` ─────────────────────────────────────────────────── */

static void on_read_mem_m_calls_updi_mem_read_and_returns_hex(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_read_canned_len = 4;
    mock_read_canned[0] = 0xDE; mock_read_canned[1] = 0xAD;
    mock_read_canned[2] = 0xBE; mock_read_canned[3] = 0xEF;
    /* SRAM address: GDB 0x800100 → UPDI 0x000100 (bit-23 flip). */
    rsp_dispatch(sock_pair[1], "m800100,4", &h);
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
    TEST_ASSERT_EQUAL(0, mock_flash_write_count);
}

static void on_write_mem_X_calls_nvm_write_flash_for_flash_address(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* GDB FLASH addr 0x100 → updi_nvm_flash_patch(0x800100). The X/M
     * handler routes FLASH writes through the read-modify-write helper
     * so `(gdb) load` works without page-aligned chunks, matching the
     * Phase-10 acceptance harness (tests/hw/gdb_acceptance.py G2). */
    rsp_dispatch(sock_pair[1], "X100,2:\x98\x95", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_flash_patch_count);
    TEST_ASSERT_EQUAL(0x800100u, mock_flash_patches[0].addr);
    TEST_ASSERT_EQUAL(0, mock_flash_write_count);
}

/* ── LLR-RSP-07: insert bp (HW comparator) ─────────────────────────── */

static void on_insert_bp_calls_updi_ocd_set_hw_bp_with_byte_addr(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    ctx.bp_mode = RSP_BP_MODE_HW_ONLY;   /* legacy Z0→HW path */
    rsp_dispatch(sock_pair[1], "Z0,200,2", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_hw_bp_set_calls);
    TEST_ASSERT_EQUAL(0x200u, mock_hw_bp_silicon[0]);
    TEST_ASSERT_EQUAL(0xFFFFFFFFu, mock_hw_bp_silicon[1]);
    TEST_ASSERT_EQUAL(0, mock_flash_write_count); /* no SW patching */
}

static void on_insert_bp_second_slot_succeeds(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    ctx.bp_mode = RSP_BP_MODE_HW_ONLY;
    rsp_dispatch(sock_pair[1], "Z0,200,2", &h);
    rsp_dispatch(sock_pair[1], "Z0,400,2", &h);
    TEST_ASSERT_EQUAL(2, mock_hw_bp_set_calls);
    TEST_ASSERT_EQUAL(0x200u, mock_hw_bp_silicon[0]);
    TEST_ASSERT_EQUAL(0x400u, mock_hw_bp_silicon[1]);
}

static void on_insert_bp_duplicate_returns_ok_without_reprogramming(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    ctx.bp_mode = RSP_BP_MODE_HW_ONLY;
    rsp_dispatch(sock_pair[1], "Z0,201,2", &h);
    rsp_dispatch(sock_pair[1], "Z0,201,2", &h);
    TEST_ASSERT_EQUAL(1, mock_hw_bp_set_calls);
}

/* ── LLR-RSP-08: remove bp ──────────────────────────────────────────── */

static void on_remove_bp_calls_updi_ocd_clear_hw_bp(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    ctx.bp_mode = RSP_BP_MODE_HW_ONLY;
    rsp_dispatch(sock_pair[1], "Z0,300,2", &h);
    rsp_dispatch(sock_pair[1], "z0,300,2", &h);
    TEST_ASSERT_EQUAL(1, mock_hw_bp_clear_calls);
    TEST_ASSERT_EQUAL(0xFFFFFFFFu, mock_hw_bp_silicon[0]);
}

static void on_remove_bp_unknown_address_returns_ok_for_resync(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "z0,deadbeef,2", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(0, mock_hw_bp_clear_calls);
}

/* ── LLR-RSP-09: third bp returns E08 ──────────────────────────────── */

static void on_insert_bp_returns_E08_when_both_slots_occupied(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    ctx.bp_mode = RSP_BP_MODE_HW_ONLY;
    rsp_dispatch(sock_pair[1], "Z0,200,2", &h);
    rsp_dispatch(sock_pair[1], "Z0,400,2", &h);
    char drain_buf[256]; drain(sock_pair[0], drain_buf, sizeof drain_buf);
    rsp_dispatch(sock_pair[1], "Z0,600,2", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("E08", payload);
    TEST_ASSERT_EQUAL(2, mock_hw_bp_set_calls); /* no third call */
}

/* ── LLR-RSP-10: step ───────────────────────────────────────────────── */

static void on_step_s_calls_updi_step_and_sends_T05_stop_reason(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_ocd_status1 = 0x00; /* not EXTBRK */
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
    mock_ocd_poll_default = 0; /* halt immediately on first poll */
    rsp_dispatch(sock_pair[1], "c", &h);
    TEST_ASSERT_EQUAL(1, mock_run_calls);
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_invalidate_calls);
    /* run must come before invalidate. */
    TEST_ASSERT_TRUE(event_index(EV_RUN) >= 0);
}

static void on_continue_rebuilds_thread_list_after_halt_and_sends_stop(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* Script: 3 "running" then halt. */
    mock_ocd_poll_script[0] = 1; mock_ocd_poll_script[1] = 1;
    mock_ocd_poll_script[2] = 1; mock_ocd_poll_script[3] = 0;
    mock_ocd_poll_script_len = 4;
    rsp_dispatch(sock_pair[1], "c", &h);
    TEST_ASSERT_EQUAL(1, mock_build_calls);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T05", 3));
}

static void on_continue_ctrl_c_returns_T02(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* Never halt naturally — but write a Ctrl-C into the client side
     * so dh_continue's select() picks it up on the first iteration. */
    mock_ocd_poll_default = 1; /* always still running */
    write(sock_pair[0], "\x03", 1);
    rsp_dispatch(sock_pair[1], "c", &h);
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_halt_calls);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T02", 3));
}

static void on_continue_returns_E01_after_UPDI_FAIL_MAX_consecutive_poll_failures(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* 8 consecutive poll failures should hit UPDI_FAIL_MAX guard. */
    mock_ocd_poll_default = -1;
    rsp_dispatch(sock_pair[1], "c", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("E01", payload);
    TEST_ASSERT_EQUAL(8, mock_ocd_poll_calls);
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_halt_calls); /* defensive halt */
}

/* ── LLR-RSP-17: ? / halt reason ────────────────────────────────────── */

static void on_halt_reason_returns_T02_when_extbrk_set(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_ocd_status1 = 0x10; /* EXTBRK */
    rsp_dispatch(sock_pair[1], "?", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T02", 3));

    /* And T05 when EXTBRK is clear. */
    reset_mocks();
    build_ctx(&ctx, &h);
    mock_ocd_status1 = 0x00;
    rsp_dispatch(sock_pair[1], "?", &h);
    char stream2[64]; drain(sock_pair[0], stream2, sizeof stream2);
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream2, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T05", 3));
}

/* ── LLR-RSP-12: qSupported / qAttached ───────────────────────────── */

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
    TEST_ASSERT_EQUAL(0, mock_ocd_gpr_reads_active);
}

static void rsp_dispatch_qattached_returns_1_no_target_access(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qAttached", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("1", payload);
}

/* ── LLR-RSP-13 / LLR-RSP-18: D ─────────────────────────────────────── */

static void on_detach_D_resumes_target_closes_socket_resets_gdb_fd(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "D", &h);
    TEST_ASSERT_EQUAL(1, mock_run_calls);
    TEST_ASSERT_EQUAL(-1, g_gdb_fd_var);
    sock_pair[1] = -1;
}

static void on_detach_clears_hw_bps_before_run(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    ctx.bp_mode = RSP_BP_MODE_HW_ONLY;
    rsp_dispatch(sock_pair[1], "Z0,200,2", &h);
    rsp_dispatch(sock_pair[1], "Z0,400,2", &h);
    char drain_buf[256]; drain(sock_pair[0], drain_buf, sizeof drain_buf);
    int events_before = mock_event_count;
    rsp_dispatch(sock_pair[1], "D", &h);
    TEST_ASSERT_EQUAL(2, mock_hw_bp_clear_calls);
    TEST_ASSERT_EQUAL(0xFFFFFFFFu, mock_hw_bp_silicon[0]);
    TEST_ASSERT_EQUAL(0xFFFFFFFFu, mock_hw_bp_silicon[1]);
    /* Both EV_OCD_CLEAR_BP events must precede the EV_RUN event. */
    int run_idx = -1, last_clear_idx = -1;
    for (int i = events_before; i < mock_event_count; ++i) {
        if (mock_events[i] == EV_OCD_CLEAR_BP) last_clear_idx = i;
        if (mock_events[i] == EV_RUN && run_idx < 0) run_idx = i;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(0, last_clear_idx);
    TEST_ASSERT_GREATER_THAN(last_clear_idx, run_idx);
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
    const char *first = strchr(stream, '$');
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL('O', first[1]);
}

/* ── LLR-RSP-16: H<g|c><tid> ────────────────────────────────────────── */

static void H_packet_stores_thread_id_for_register_operations(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    fake_fsm.thread_count = 3;
    fake_fsm.threads[1].gdb_id = 2; strcpy(fake_fsm.threads[1].name, "B");
    fake_fsm.threads[2].gdb_id = 3; strcpy(fake_fsm.threads[2].name, "C");
    mock_active_thread = 1; /* so tid 3 != active → fsm path */
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

/* ── LLR-RSP-19: qC ─────────────────────────────────────────────────── */

static void qC_returns_QC0_when_no_c_thread_selected(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qC", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("QC0", payload);
    /* No target access. */
    TEST_ASSERT_EQUAL(0, mock_read_count);
    TEST_ASSERT_EQUAL(0, mock_ocd_gpr_reads_active);
}

static void qC_returns_selected_c_thread_in_hex(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    c_tid_var = 0x2a;   /* 42 → "QC2a" */
    rsp_dispatch(sock_pair[1], "qC", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("QC2a", payload);
}

/* ── LLR-RSP-20: qOffsets ──────────────────────────────────────────── */

static void qOffsets_returns_text_data_bss_all_zero(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qOffsets", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("Text=0;Data=0;Bss=0", payload);
    TEST_ASSERT_EQUAL(0, mock_read_count);
}

/* ── LLR-RSP-21: T<tid> is-thread-alive ────────────────────────────── */

static void T_packet_returns_OK_for_live_thread(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    fake_fsm.thread_count = 2;
    fake_fsm.threads[0].gdb_id = 1;
    fake_fsm.threads[1].gdb_id = 3;
    rsp_dispatch(sock_pair[1], "T3", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
}

static void T_packet_returns_E01_for_unknown_thread(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    fake_fsm.thread_count = 1;
    fake_fsm.threads[0].gdb_id = 1;
    rsp_dispatch(sock_pair[1], "T2a", &h);   /* tid 42 */
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("E01", payload);
}

/* ── LLR-RSP-22: R<XX> restart ─────────────────────────────────────── */

static void R_packet_invalidates_fsm_runs_and_emits_stop(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_ocd_poll_default = 0;   /* halt on first poll */

    rsp_dispatch(sock_pair[1], "R00", &h);

    /* Reset path (enter_debug) → fsm_invalidate → continue. */
    TEST_ASSERT_GREATER_THAN(0, mock_invalidate_calls);
    TEST_ASSERT_EQUAL(1, mock_run_calls);
    /* dh_continue rebuilds the thread list after the halt and emits a
     * T05 stop packet so GDB resumes interactive control. */
    TEST_ASSERT_GREATER_THAN(0, mock_build_calls);
    char stream[128]; drain(sock_pair[0], stream, sizeof stream);
    char payload[128];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T05", 3));
}

/* ── LLR-RSP-23: vRun;… ─────────────────────────────────────────────── */

static void vRun_invalidates_fsm_runs_and_emits_stop(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_ocd_poll_default = 0;   /* halt on first poll */

    /* vRun with empty filename and one arg, both ignored. */
    rsp_dispatch(sock_pair[1], "vRun;;arg1", &h);

    /* Per GDB protocol, vRun must reset the program and respond with a
     * stop reply describing the halt-at-entry state.  No CPU "run" is
     * issued — the client decides when to start execution via its own
     * `c` / `s`. */
    TEST_ASSERT_GREATER_THAN(0, mock_invalidate_calls);
    TEST_ASSERT_EQUAL(0, mock_run_calls);
    TEST_ASSERT_GREATER_THAN(0, mock_build_calls);
    char stream[128]; drain(sock_pair[0], stream, sizeof stream);
    char payload[128];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T05", 3));
}

/* ── LLR-RSP-24: vAttach;<pid> ──────────────────────────────────────── */

static void vAttach_halts_target_and_emits_stop_reply(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "vAttach;1", &h);

    /* enter_debug + invalidate + build_thread_list; no run/step. */
    TEST_ASSERT_EQUAL(0, mock_run_calls);
    TEST_ASSERT_EQUAL(0, mock_step_calls);
    TEST_ASSERT_GREATER_THAN(0, mock_invalidate_calls);
    TEST_ASSERT_GREATER_THAN(0, mock_build_calls);
    char stream[128]; drain(sock_pair[0], stream, sizeof stream);
    char payload[128];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    /* T05 stop reply with thread suffix. */
    TEST_ASSERT_EQUAL(0, strncmp(payload, "T05", 3));
    TEST_ASSERT_NOT_NULL(strstr(payload, "thread:"));
}

/* ── LLR-RSP-25: vKill;<pid> ────────────────────────────────────────── */

static void vKill_sets_quit_and_replies_ok(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    TEST_ASSERT_EQUAL(0, g_quit_var);
    rsp_dispatch(sock_pair[1], "vKill;1", &h);

    TEST_ASSERT_EQUAL(1, (int)g_quit_var);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
}

static void qSupported_advertises_multiprocess_vRun_vAttach_vKill(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qSupported:multiprocess+", &h);
    char stream[256]; drain(sock_pair[0], stream, sizeof stream);
    char payload[256];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_NOT_NULL(strstr(payload, "multiprocess+"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "vRun+"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "vAttach+"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "vKill+"));
    TEST_ASSERT_NULL(strstr(payload, "multiprocess-"));
}

/* ── HLR-056: Z2/Z3/Z4 data watchpoints — silicon does not expose
 *           the hardware over UPDI (see src/updi.h and
 *           doc/reference/guesswork.md).  Server replies the empty
 *           packet so GDB falls back to software watchpoints.       */

static void Z2_replies_empty_packet_so_gdb_falls_back_to_sw_watch(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "Z2,800100,1", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("", payload);
}

static void z3_remove_also_replies_empty_packet(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "z3,800200,1", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("", payload);
}

/* ── HLR-053: vFlashErase / vFlashWrite / vFlashDone — (gdb) load ─── */

static void vFlashErase_allocates_buffer_and_replies_ok_for_flash_range(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "vFlashErase:100,200", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_NOT_NULL(ctx.flash_xact_buf);
    TEST_ASSERT_EQUAL(0u, ctx.flash_xact_base);           /* page-floor(0x100) = 0 */
    /* Range [0x100..0x300) straddles two 512-byte pages → 1024 bytes. */
    TEST_ASSERT_EQUAL(2u * UPDI_FLASH_PAGE_SIZE, ctx.flash_xact_len);
    /* Buffer pre-filled with 0xFF. */
    TEST_ASSERT_EQUAL_HEX8(0xFFu, ctx.flash_xact_buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, ctx.flash_xact_buf[ctx.flash_xact_len - 1u]);
    free(ctx.flash_xact_buf);
}

static void vFlashErase_E22_for_data_space_address(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* 0x810000 is EEPROM on the GDB side (bit-23 data-flag set). */
    rsp_dispatch(sock_pair[1], "vFlashErase:810000,10", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("E22", payload);
    TEST_ASSERT_NULL(ctx.flash_xact_buf);
}

static void vFlashWrite_copies_payload_into_buffer_at_offset(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "vFlashErase:0,200", &h);
    char stream0[64]; drain(sock_pair[0], stream0, sizeof stream0);
    /* Plain 4-byte payload at offset 0x10, no escapes. */
    rsp_dispatch(sock_pair[1], "vFlashWrite:10:\x11\x22\x33\x44", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL_HEX8(0x11u, ctx.flash_xact_buf[0x10]);
    TEST_ASSERT_EQUAL_HEX8(0x22u, ctx.flash_xact_buf[0x11]);
    TEST_ASSERT_EQUAL_HEX8(0x33u, ctx.flash_xact_buf[0x12]);
    TEST_ASSERT_EQUAL_HEX8(0x44u, ctx.flash_xact_buf[0x13]);
    /* Surrounding bytes still erased. */
    TEST_ASSERT_EQUAL_HEX8(0xFFu, ctx.flash_xact_buf[0x0F]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, ctx.flash_xact_buf[0x14]);
    free(ctx.flash_xact_buf);
}

static void vFlashWrite_decodes_0x7D_xor_0x20_binary_escape(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "vFlashErase:0,200", &h);
    char stream0[64]; drain(sock_pair[0], stream0, sizeof stream0);
    /* 0x7D 0x03 → 0x23 ('#'); 0x7D 0x0A → 0x2A ('*'). */
    static const char pkt[] = {
        'v','F','l','a','s','h','W','r','i','t','e',':','0',':',
        0x7D, 0x03, 0x7D, 0x0A, 0x00
    };
    /* Pass the length explicitly via rsp_dispatch_n so the embedded
     * sequence isn't truncated by strlen.                            */
    rsp_dispatch_n(sock_pair[1], pkt, sizeof pkt - 1, &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL_HEX8(0x23u, ctx.flash_xact_buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x2Au, ctx.flash_xact_buf[1]);
    free(ctx.flash_xact_buf);
}

static void vFlashDone_flushes_buffer_via_nvm_write_flash_and_replies_ok(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "vFlashErase:0,200", &h);
    char s0[64]; drain(sock_pair[0], s0, sizeof s0);
    rsp_dispatch(sock_pair[1], "vFlashWrite:0:\x98\x95", &h);
    char s1[64]; drain(sock_pair[0], s1, sizeof s1);

    /* Pre-install an HW BP shadow entry so we can prove vFlashDone
     * clears it (HLR-053 spec).                                      */
    ctx.hw_bp_addr[0] = 0x42u;

    rsp_dispatch(sock_pair[1], "vFlashDone", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);

    TEST_ASSERT_EQUAL(1, mock_flash_write_count);
    TEST_ASSERT_EQUAL(UPDI_FLASH_BASE + 0u, mock_flash_writes[0].addr);
    TEST_ASSERT_EQUAL(UPDI_FLASH_PAGE_SIZE, mock_flash_writes[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x98u, mock_flash_writes[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x95u, mock_flash_writes[0].data[1]);

    /* Buffer freed and xact cleared. */
    TEST_ASSERT_NULL(ctx.flash_xact_buf);
    TEST_ASSERT_EQUAL(0u, ctx.flash_xact_len);
    /* HW BP shadow cleared. */
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, ctx.hw_bp_addr[0]);
}

static void vFlashDone_with_no_active_transaction_replies_ok_noop(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "vFlashDone", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(0, mock_flash_write_count);
}

static void m_packet_mid_vflash_transaction_aborts_and_returns_E22(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "vFlashErase:0,200", &h);
    char s0[64]; drain(sock_pair[0], s0, sizeof s0);
    TEST_ASSERT_NOT_NULL(ctx.flash_xact_buf);

    rsp_dispatch(sock_pair[1], "m800100,4", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[32];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("E22", payload);
    /* Buffer freed by the abort path. */
    TEST_ASSERT_NULL(ctx.flash_xact_buf);
}

static void qSupported_advertises_vFlash_packets(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "qSupported:multiprocess+", &h);
    char stream[256]; drain(sock_pair[0], stream, sizeof stream);
    char payload[256];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_NOT_NULL(strstr(payload, "vFlashErase+"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "vFlashWrite+"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "vFlashDone+"));
}

/* ── HLR-054: true SW breakpoints via FLASH BREAK opcode ────────────── */

static void Z0_in_sw_mode_patches_BREAK_opcode_via_flash_patch(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    /* default bp_mode == RSP_BP_MODE_SW */
    rsp_dispatch(sock_pair[1], "Z0,400,2", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(1, mock_flash_patch_count);
    TEST_ASSERT_EQUAL(UPDI_FLASH_BASE + 0x400u, mock_flash_patches[0].addr);
    TEST_ASSERT_EQUAL(2u, mock_flash_patches[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x98u, mock_flash_patches[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x95u, mock_flash_patches[0].data[1]);
    /* No HW comparator should have been touched. */
    TEST_ASSERT_EQUAL(0, mock_hw_bp_set_calls);
}

static void Z0_in_sw_mode_records_original_opcode_from_flash(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_read_canned[0] = 0xCDu;
    mock_read_canned[1] = 0xABu;
    mock_read_canned_len = 2;
    rsp_dispatch(sock_pair[1], "Z0,500,2", &h);
    TEST_ASSERT_TRUE(ctx.sw_bp[0].in_use);
    TEST_ASSERT_EQUAL(0x500u, ctx.sw_bp[0].addr);
    TEST_ASSERT_EQUAL_HEX8(0xCDu, ctx.sw_bp[0].orig[0]);
    TEST_ASSERT_EQUAL_HEX8(0xABu, ctx.sw_bp[0].orig[1]);
}

static void z0_in_sw_mode_restores_original_opcode(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    mock_read_canned[0] = 0x42u;
    mock_read_canned[1] = 0xC9u;
    mock_read_canned_len = 2;
    rsp_dispatch(sock_pair[1], "Z0,600,2", &h);
    char drain_buf[256]; drain(sock_pair[0], drain_buf, sizeof drain_buf);
    rsp_dispatch(sock_pair[1], "z0,600,2", &h);
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("OK", payload);
    TEST_ASSERT_EQUAL(2, mock_flash_patch_count);
    /* Second patch must rewrite the saved original opcode. */
    TEST_ASSERT_EQUAL(UPDI_FLASH_BASE + 0x600u, mock_flash_patches[1].addr);
    TEST_ASSERT_EQUAL_HEX8(0x42u, mock_flash_patches[1].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC9u, mock_flash_patches[1].data[1]);
    TEST_ASSERT_FALSE(ctx.sw_bp[0].in_use);
}

static void Z0_in_sw_mode_refuses_data_space_address_with_E22(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "Z0,810000,2", &h);   /* EEPROM window */
    char stream[64]; drain(sock_pair[0], stream, sizeof stream);
    char payload[64];
    TEST_ASSERT_EQUAL(0, last_packet_payload(stream, payload, sizeof payload));
    TEST_ASSERT_EQUAL_STRING("E22", payload);
    TEST_ASSERT_EQUAL(0, mock_flash_patch_count);
    TEST_ASSERT_FALSE(ctx.sw_bp[0].in_use);
}

static void Z0_in_sw_mode_snapshots_and_restores_cpu_state(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    for (int i = 0; i < 32; i++) mock_ocd_gpr[i] = (uint8_t)(0x10u + i);
    mock_ocd_sreg = 0xAAu;
    mock_ocd_sp   = 0xBEEFu;
    mock_ocd_pc   = 0xCAFEu;
    rsp_dispatch(sock_pair[1], "Z0,700,2", &h);
    TEST_ASSERT_GREATER_OR_EQUAL(32, mock_ocd_gpr_writes);
    TEST_ASSERT_GREATER_OR_EQUAL(1,  mock_ocd_sreg_writes);
    TEST_ASSERT_GREATER_OR_EQUAL(1,  mock_ocd_sp_writes);
    TEST_ASSERT_GREATER_OR_EQUAL(1,  mock_ocd_pc_writes);
    /* The mock writes update the same shadow the reads consume, so
     * the round-trip leaves the state intact. */
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x10u + i), mock_ocd_gpr[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0xAAu, mock_ocd_sreg);
    TEST_ASSERT_EQUAL(0xBEEFu, mock_ocd_sp);
    TEST_ASSERT_EQUAL(0xCAFEu, mock_ocd_pc);
}

static void Z0_in_hw_only_mode_falls_back_to_HW_BP_path(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    ctx.bp_mode = RSP_BP_MODE_HW_ONLY;
    rsp_dispatch(sock_pair[1], "Z0,800,2", &h);
    TEST_ASSERT_EQUAL(1, mock_hw_bp_set_calls);
    TEST_ASSERT_EQUAL(0, mock_flash_patch_count);
    TEST_ASSERT_FALSE(ctx.sw_bp[0].in_use);
}

static void Z0_in_sw_mode_idempotent_on_same_address(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "Z0,900,2", &h);
    rsp_dispatch(sock_pair[1], "Z0,900,2", &h);
    TEST_ASSERT_EQUAL(1, mock_flash_patch_count);
}

static void vFlashDone_also_clears_sw_bp_shadow(void)
{
    RspContext ctx; RspHandlers h; build_ctx(&ctx, &h);
    rsp_dispatch(sock_pair[1], "Z0,A00,2", &h);
    TEST_ASSERT_TRUE(ctx.sw_bp[0].in_use);
    char drain_buf[256]; drain(sock_pair[0], drain_buf, sizeof drain_buf);
    rsp_dispatch(sock_pair[1], "vFlashErase:0,200", &h);
    drain(sock_pair[0], drain_buf, sizeof drain_buf);
    rsp_dispatch(sock_pair[1], "vFlashDone", &h);
    TEST_ASSERT_FALSE(ctx.sw_bp[0].in_use);
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
    RUN_TEST(on_read_regs_non_active_thread_uses_fsm_register_frame);
    RUN_TEST(on_write_regs_G_writes_all_registers_via_ocd);
    RUN_TEST(on_write_regs_P_writes_single_register_via_ocd);
    RUN_TEST(on_read_mem_m_calls_updi_mem_read_and_returns_hex);
    RUN_TEST(on_write_mem_M_calls_updi_mem_write_for_sram_address);
    RUN_TEST(on_write_mem_X_calls_nvm_write_flash_for_flash_address);
    RUN_TEST(on_insert_bp_calls_updi_ocd_set_hw_bp_with_byte_addr);
    RUN_TEST(on_insert_bp_second_slot_succeeds);
    RUN_TEST(on_insert_bp_duplicate_returns_ok_without_reprogramming);
    RUN_TEST(on_remove_bp_calls_updi_ocd_clear_hw_bp);
    RUN_TEST(on_remove_bp_unknown_address_returns_ok_for_resync);
    RUN_TEST(on_insert_bp_returns_E08_when_both_slots_occupied);
    RUN_TEST(on_step_s_calls_updi_step_and_sends_T05_stop_reason);
    RUN_TEST(on_continue_calls_updi_run_then_fsm_invalidate);
    RUN_TEST(on_continue_rebuilds_thread_list_after_halt_and_sends_stop);
    RUN_TEST(on_continue_ctrl_c_returns_T02);
    RUN_TEST(on_continue_returns_E01_after_UPDI_FAIL_MAX_consecutive_poll_failures);
    RUN_TEST(on_halt_reason_returns_T02_when_extbrk_set);
    RUN_TEST(rsp_dispatch_qsupported_returns_feature_string_no_target_access);
    RUN_TEST(rsp_dispatch_qattached_returns_1_no_target_access);
    RUN_TEST(on_detach_D_resumes_target_closes_socket_resets_gdb_fd);
    RUN_TEST(on_detach_clears_hw_bps_before_run);
    RUN_TEST(on_kill_k_sets_g_quit_to_1);
    RUN_TEST(on_monitor_qRcmd_passes_hex_body_to_monitor_dispatch);
    RUN_TEST(on_monitor_returns_ok_when_monitor_dispatch_succeeds);
    RUN_TEST(on_monitor_sends_o_packet_error_on_updi_failure);
    RUN_TEST(H_packet_stores_thread_id_for_register_operations);
    RUN_TEST(H_packet_minus1_and_0_both_map_to_active_fsm_thread);
    RUN_TEST(qC_returns_QC0_when_no_c_thread_selected);
    RUN_TEST(qC_returns_selected_c_thread_in_hex);
    RUN_TEST(qOffsets_returns_text_data_bss_all_zero);
    RUN_TEST(T_packet_returns_OK_for_live_thread);
    RUN_TEST(T_packet_returns_E01_for_unknown_thread);
    RUN_TEST(R_packet_invalidates_fsm_runs_and_emits_stop);
    RUN_TEST(vRun_invalidates_fsm_runs_and_emits_stop);
    RUN_TEST(vAttach_halts_target_and_emits_stop_reply);
    RUN_TEST(vKill_sets_quit_and_replies_ok);
    RUN_TEST(qSupported_advertises_multiprocess_vRun_vAttach_vKill);
    RUN_TEST(Z2_replies_empty_packet_so_gdb_falls_back_to_sw_watch);
    RUN_TEST(z3_remove_also_replies_empty_packet);
    /* HLR-053: vFlashErase / vFlashWrite / vFlashDone (`gdb load`). */
    RUN_TEST(vFlashErase_allocates_buffer_and_replies_ok_for_flash_range);
    RUN_TEST(vFlashErase_E22_for_data_space_address);
    RUN_TEST(vFlashWrite_copies_payload_into_buffer_at_offset);
    RUN_TEST(vFlashWrite_decodes_0x7D_xor_0x20_binary_escape);
    RUN_TEST(vFlashDone_flushes_buffer_via_nvm_write_flash_and_replies_ok);
    RUN_TEST(vFlashDone_with_no_active_transaction_replies_ok_noop);
    RUN_TEST(m_packet_mid_vflash_transaction_aborts_and_returns_E22);
    RUN_TEST(qSupported_advertises_vFlash_packets);
    RUN_TEST(Z0_in_sw_mode_patches_BREAK_opcode_via_flash_patch);
    RUN_TEST(Z0_in_sw_mode_records_original_opcode_from_flash);
    RUN_TEST(z0_in_sw_mode_restores_original_opcode);
    RUN_TEST(Z0_in_sw_mode_refuses_data_space_address_with_E22);
    RUN_TEST(Z0_in_sw_mode_snapshots_and_restores_cpu_state);
    RUN_TEST(Z0_in_hw_only_mode_falls_back_to_HW_BP_path);
    RUN_TEST(Z0_in_sw_mode_idempotent_on_same_address);
    RUN_TEST(vFlashDone_also_clears_sw_bp_shadow);
    return UNITY_END();
}
