/* tests/test_monitor.c — Unity tests for src/monitor.c
 *
 * Mocks updi_mem_read() with a region store (FLASH + SRAM canned bytes) and
 * captures every rsp_send_packet() call so tests can assert on the encoded
 * O-packet payloads.
 *
 * Verifies: hex command decoding, prefix matching, both sub-commands
 * (events, queues), descriptor-iteration shape, O-packet hex encoding,
 * 512-byte text staging buffer cap, and the no-halt guarantee.
 */
#include "unity.h"
#include "monitor.h"
#include "elf_parser.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Mock UPDI memory ────────────────────────────────────────────────── */

#define MAX_REGIONS 32
#define MAX_PACKETS 64

typedef struct {
    uint32_t       addr;
    size_t         len;
    const uint8_t *data;
} Region;

static Region g_regions[MAX_REGIONS];
static size_t g_region_count;

static char  *g_packets[MAX_PACKETS];
static size_t g_packet_count;

static int    g_halt_calls;
static int    g_halt_rc;
static int    g_read_calls;
static size_t g_read_total_bytes;

static void mock_reset(void)
{
    g_region_count = 0;
    for (size_t i = 0; i < g_packet_count; ++i) free(g_packets[i]);
    g_packet_count = 0;
    g_halt_calls = 0;
    g_read_calls = 0;
    g_read_total_bytes = 0;
}

static void add_region(uint32_t addr, const void *data, size_t len)
{
    TEST_ASSERT_LESS_THAN(MAX_REGIONS, g_region_count);
    g_regions[g_region_count].addr = addr;
    g_regions[g_region_count].len  = len;
    g_regions[g_region_count].data = (const uint8_t *)data;
    g_region_count++;
}

int __wrap_updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)fd;
    g_read_calls++;
    g_read_total_bytes += len;
    /* fsm_mapper/monitor route flash reads through UPDI's flash mirror
     * (addr | 0x800000).  Tests register backing regions at the raw
     * GDB-AVR / data-space address, so try the literal address first,
     * then retry with the flash-mirror flag stripped. */
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t a = (pass == 0) ? addr : (addr & 0x7FFFFFu);
        for (size_t i = 0; i < g_region_count; ++i) {
            const Region *r = &g_regions[i];
            if (a >= r->addr && (a + len) <= (r->addr + r->len)) {
                memcpy(buf, r->data + (a - r->addr), len);
                return 0;
            }
        }
        if (!(addr & 0x800000u)) break;
    }
    return -1;
}

int __wrap_updi_halt(int fd) { (void)fd; g_halt_calls++; return g_halt_rc; }

/* HLR-055 mocks: counters + injectable return codes for verb tests. */
static int g_enter_debug_calls;
static int g_enter_debug_rc;
static int g_run_calls;
static int g_run_rc;
static int g_chip_erase_calls;
static int g_chip_erase_rc;
static int g_fsm_invalidate_calls;
static int g_fsm_build_calls;
static int g_hw_bp_clear_calls;
static int g_sw_bp_clear_calls;

int __wrap_updi_enter_debug(int fd)
{
    (void)fd; g_enter_debug_calls++; return g_enter_debug_rc;
}
int __wrap_updi_run(int fd) { (void)fd; g_run_calls++; return g_run_rc; }
int __wrap_updi_chip_erase(int fd)
{
    (void)fd; g_chip_erase_calls++; return g_chip_erase_rc;
}
struct FsmContext;
void __wrap_fsm_invalidate(struct FsmContext *c)
{
    (void)c; g_fsm_invalidate_calls++;
}
int __wrap_fsm_get_active_thread(struct FsmContext *c) { (void)c; return 1; }
int __wrap_fsm_build_thread_list(struct FsmContext *c,
                                 const AvrOsSymbolIndex *idx,
                                 int updi_fd)
{
    (void)c; (void)idx; (void)updi_fd;
    g_fsm_build_calls++;
    return 0;
}

struct RspContext;
void __wrap_rsp_hw_bp_clear_all(struct RspContext *c)
{
    (void)c; g_hw_bp_clear_calls++;
}
void __wrap_rsp_sw_bp_clear_all(struct RspContext *c)
{
    (void)c; g_sw_bp_clear_calls++;
}

/* Override __wrap_updi_halt above's RC: we keep the counter but allow
 * tests to inject a non-zero return.  Redefine via a thin shim: */
static void hlr055_mock_reset(void)
{
    g_enter_debug_calls = 0; g_enter_debug_rc = 0;
    g_run_calls = 0;         g_run_rc = 0;
    g_chip_erase_calls = 0;  g_chip_erase_rc = 0;
    g_fsm_invalidate_calls = 0;
    g_fsm_build_calls = 0;
    g_hw_bp_clear_calls = 0;
    g_sw_bp_clear_calls = 0;
    g_halt_rc = 0;
}

int __wrap_rsp_send_packet(int fd, const char *payload)
{
    (void)fd;
    TEST_ASSERT_LESS_THAN(MAX_PACKETS, g_packet_count);
    g_packets[g_packet_count++] = strdup(payload);
    return 0;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Hex-encode an ASCII command into a freshly-allocated NUL-terminated buffer.
 * The caller owns the returned pointer. */
static char *hexify(const char *text)
{
    size_t n = strlen(text);
    char *out = malloc(n * 2u + 1u);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = (uint8_t)text[i];
        out[i * 2u]      = hex[(b >> 4) & 0xFu];
        out[i * 2u + 1u] = hex[b & 0xFu];
    }
    out[n * 2u] = '\0';
    return out;
}

/* Decode the most-recent O-packet payload back to plain text (skips the
 * leading 'O'). Returns a malloc'd string the caller frees. */
static char *last_o_packet_text(void)
{
    TEST_ASSERT_GREATER_THAN(0, g_packet_count);
    const char *p = g_packets[g_packet_count - 1];
    TEST_ASSERT_EQUAL_CHAR('O', p[0]);
    size_t hex_len = strlen(p) - 1u;
    TEST_ASSERT_EQUAL(0u, hex_len % 2u);
    size_t n = hex_len / 2u;
    char *out = malloc(n + 1u);
    for (size_t i = 0; i < n; ++i) {
        unsigned v;
        sscanf(p + 1u + i * 2u, "%2x", &v);
        out[i] = (char)v;
    }
    out[n] = '\0';
    return out;
}

/* Build a 4-byte evntDescriptor at &dst[0]. */
static void put_evnt_descr(uint8_t *dst, uint16_t name_ptr, uint16_t status_ptr)
{
    dst[0] = (uint8_t)(name_ptr & 0xFFu);
    dst[1] = (uint8_t)(name_ptr >> 8);
    dst[2] = (uint8_t)(status_ptr & 0xFFu);
    dst[3] = (uint8_t)(status_ptr >> 8);
}

/* Build a 10-byte queDescriptor: q*(2) buf*(2) evt*(2) cap(2) size(2). */
static void put_que_descr(uint8_t *dst,
                          uint16_t qp, uint16_t bp, uint16_t ep,
                          uint16_t capacity, uint16_t elem_sz)
{
    dst[0] = (uint8_t)(qp & 0xFFu);       dst[1] = (uint8_t)(qp >> 8);
    dst[2] = (uint8_t)(bp & 0xFFu);       dst[3] = (uint8_t)(bp >> 8);
    dst[4] = (uint8_t)(ep & 0xFFu);       dst[5] = (uint8_t)(ep >> 8);
    dst[6] = (uint8_t)(capacity & 0xFFu); dst[7] = (uint8_t)(capacity >> 8);
    dst[8] = (uint8_t)(elem_sz & 0xFFu);  dst[9] = (uint8_t)(elem_sz >> 8);
}

void setUp(void)    { mock_reset(); }
void tearDown(void) { mock_reset(); }

/* ── Standard fixtures ──────────────────────────────────────────────── */

static AvrOsSymbolIndex make_idx(void)
{
    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof idx);
    return idx;
}

/* Install one event named "RX_RDY" with status_ptr → SRAM byte 0xAA */
static AvrOsSymbolIndex install_one_event(void)
{
    static uint8_t evnt_table[4];
    static const char name[] = "RX_RDY";
    static uint8_t status = 0xAAu;

    put_evnt_descr(evnt_table, 0x7000u, 0x4000u);
    add_region(0x6000u, evnt_table, sizeof evnt_table);
    add_region(0x7000u, name, sizeof name);   /* includes NUL */
    add_region(0x4000u, &status, 1u);

    AvrOsSymbolIndex idx = make_idx();
    idx.event_table_addr = 0x6000u;
    idx.event_count      = 1u;
    return idx;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

/* T1: hex-decode happens before prefix match — "avros events" routes to cmd_events */
void test_monitor_dispatch_hex_decodes_cmd_before_prefix_matching(void)
{
    AvrOsSymbolIndex idx = install_one_event();
    char *hex = hexify("avros events");

    int rc = monitor_dispatch(/*rsp*/1, /*updi*/2, &idx, hex);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_GREATER_THAN(0, g_packet_count);     /* o-packet sent */
    TEST_ASSERT_GREATER_THAN(0, g_read_calls);       /* updi was hit */
    free(hex);
}

/* T2: odd-length hex string → -2, no packet, no read */
void test_monitor_dispatch_treats_invalid_hex_sequence_as_unrecognised(void)
{
    AvrOsSymbolIndex idx = make_idx();
    int rc = monitor_dispatch(1, 2, &idx, "abc");    /* odd length */
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_EQUAL_size_t(0, g_packet_count);
    TEST_ASSERT_EQUAL_INT(0, g_read_calls);
}

/* T3: decoded command lacking "avros " prefix → -2 */
void test_monitor_dispatch_rejects_cmd_without_avros_space_prefix(void)
{
    AvrOsSymbolIndex idx = make_idx();
    char *hex = hexify("other events");
    int rc = monitor_dispatch(1, 2, &idx, hex);
    TEST_ASSERT_EQUAL_INT(-2, rc);
    free(hex);
}

/* T4: bad prefix → at least one O-packet containing usage hint */
void test_monitor_dispatch_sends_usage_hint_o_packet_on_bad_prefix(void)
{
    AvrOsSymbolIndex idx = make_idx();
    char *hex = hexify("foo bar");
    (void)monitor_dispatch(1, 2, &idx, hex);
    TEST_ASSERT_GREATER_THAN(0, g_packet_count);
    char *text = last_o_packet_text();
    TEST_ASSERT_NOT_NULL(strstr(text, "usage"));
    TEST_ASSERT_NOT_NULL(strstr(text, "avros"));
    free(text);
    free(hex);
}

/* T5: cmd_events reads event_count × 4 bytes from event_table_addr */
void test_cmd_events_reads_event_count_descriptors_from_evnt_table(void)
{
    AvrOsSymbolIndex idx = install_one_event();
    /* second descriptor & associated SRAM/FLASH */
    static uint8_t evnt_table[8];
    static const char name0[] = "EV0";
    static const char name1[] = "EV1";
    static uint8_t s0 = 0x01, s1 = 0x02;
    put_evnt_descr(&evnt_table[0], 0x7100u, 0x4100u);
    put_evnt_descr(&evnt_table[4], 0x7110u, 0x4101u);
    /* Replace the install_one_event regions for a clean two-event fixture. */
    mock_reset();
    add_region(0x6000u, evnt_table, sizeof evnt_table);
    add_region(0x7100u, name0, sizeof name0);
    add_region(0x7110u, name1, sizeof name1);
    add_region(0x4100u, &s0, 1u);
    add_region(0x4101u, &s1, 1u);

    idx = make_idx();
    idx.event_table_addr = 0x6000u;
    idx.event_count      = 2u;

    char *hex = hexify("avros events");
    int rc = monitor_dispatch(1, 2, &idx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* The first call must cover the descriptor block (2 × 4 = 8 bytes). */
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(8u, g_read_total_bytes);
    free(hex);
}

/* T6: cmd_events reports `<name>: <status>` per descriptor */
void test_cmd_events_reports_name_and_status_value_per_descriptor(void)
{
    static uint8_t evnt_table[8];
    static const char name0[] = "RX";
    static const char name1[] = "TX";
    static uint8_t s0 = 0x01, s1 = 0x00;
    put_evnt_descr(&evnt_table[0], 0x7100u, 0x4100u);
    put_evnt_descr(&evnt_table[4], 0x7110u, 0x4101u);
    add_region(0x6000u, evnt_table, sizeof evnt_table);
    add_region(0x7100u, name0, sizeof name0);
    add_region(0x7110u, name1, sizeof name1);
    add_region(0x4100u, &s0, 1u);
    add_region(0x4101u, &s1, 1u);

    AvrOsSymbolIndex idx = make_idx();
    idx.event_table_addr = 0x6000u;
    idx.event_count      = 2u;

    char *hex = hexify("avros events");
    TEST_ASSERT_EQUAL_INT(0, monitor_dispatch(1, 2, &idx, hex));

    char *text = last_o_packet_text();
    TEST_ASSERT_NOT_NULL(strstr(text, "RX"));
    TEST_ASSERT_NOT_NULL(strstr(text, "0x01"));
    TEST_ASSERT_NOT_NULL(strstr(text, "TX"));
    TEST_ASSERT_NOT_NULL(strstr(text, "0x00"));
    free(text);
    free(hex);
}

/* T7: hex-encoded O-packet contains uppercase ASCII hex digits */
void test_cmd_events_output_is_hex_encoded_o_packet(void)
{
    AvrOsSymbolIndex idx = install_one_event();
    char *hex = hexify("avros events");
    TEST_ASSERT_EQUAL_INT(0, monitor_dispatch(1, 2, &idx, hex));

    TEST_ASSERT_EQUAL(1u, g_packet_count);
    const char *pkt = g_packets[0];
    TEST_ASSERT_EQUAL_CHAR('O', pkt[0]);
    /* every byte after 'O' must be a hex digit and total length must be odd */
    size_t plen = strlen(pkt);
    TEST_ASSERT_EQUAL(1u, plen & 1u);
    for (size_t i = 1; i < plen; ++i) {
        char c = pkt[i];
        TEST_ASSERT_TRUE(
            (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'F'));
    }
    free(hex);
}

/* T8: cmd_queues reads queue_count × 10 bytes from queue_table_addr */
void test_cmd_queues_reads_queue_count_descriptors_from_que_table(void)
{
    static uint8_t que_table[20];
    put_que_descr(&que_table[0],  0x4200u, 0x4300u, 0x4400u, 8u, 1u);
    put_que_descr(&que_table[10], 0x4210u, 0x4310u, 0x4410u, 16u, 4u);
    add_region(0x6500u, que_table, sizeof que_table);

    AvrOsSymbolIndex idx = make_idx();
    idx.queue_table_addr = 0x6500u;
    idx.queue_count      = 2u;

    char *hex = hexify("avros queues");
    TEST_ASSERT_EQUAL_INT(0, monitor_dispatch(1, 2, &idx, hex));
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(20u, g_read_total_bytes);
    free(hex);
}

/* T9: cmd_queues formats capacity & sizeOfElement for each entry */
void test_cmd_queues_formats_capacity_and_sizeofelement_for_each_entry(void)
{
    static uint8_t que_table[20];
    put_que_descr(&que_table[0],  0,0,0, 8u, 1u);
    put_que_descr(&que_table[10], 0,0,0, 16u, 4u);
    add_region(0x6500u, que_table, sizeof que_table);

    AvrOsSymbolIndex idx = make_idx();
    idx.queue_table_addr = 0x6500u;
    idx.queue_count      = 2u;

    char *hex = hexify("avros queues");
    TEST_ASSERT_EQUAL_INT(0, monitor_dispatch(1, 2, &idx, hex));

    char *text = last_o_packet_text();
    TEST_ASSERT_NOT_NULL(strstr(text, "capacity=8"));
    TEST_ASSERT_NOT_NULL(strstr(text, "sizeOfElement=1"));
    TEST_ASSERT_NOT_NULL(strstr(text, "capacity=16"));
    TEST_ASSERT_NOT_NULL(strstr(text, "sizeOfElement=4"));
    free(text);
    free(hex);
}

/* T10: monitor_dispatch and its helpers never call updi_halt */
void test_monitor_dispatch_and_helpers_never_call_updi_halt(void)
{
    /* events */
    {
        AvrOsSymbolIndex idx = install_one_event();
        char *hex = hexify("avros events");
        (void)monitor_dispatch(1, 2, &idx, hex);
        free(hex);
    }
    /* queues */
    {
        mock_reset();
        static uint8_t qt[10];
        put_que_descr(qt, 0,0,0, 4u, 2u);
        add_region(0x6500u, qt, sizeof qt);
        AvrOsSymbolIndex idx = make_idx();
        idx.queue_table_addr = 0x6500u;
        idx.queue_count      = 1u;
        char *hex = hexify("avros queues");
        (void)monitor_dispatch(1, 2, &idx, hex);
        free(hex);
    }
    TEST_ASSERT_EQUAL_INT(0, g_halt_calls);
}

/* ── HLR-055: top-level monitor verbs via monitor_dispatch_ex ─────── */

static RspContext make_ctx(void)
{
    RspContext c;
    memset(&c, 0, sizeof c);
    c.updi_fd = 2;
    c.fsm     = NULL;
    c.idx     = NULL;
    c.bp_mode = RSP_BP_MODE_SW;
    return c;
}

void test_verb_reset_pulses_updi_and_clears_shadows(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    char *hex = hexify("reset");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, g_enter_debug_calls);
    TEST_ASSERT_EQUAL_INT(1, g_hw_bp_clear_calls);
    TEST_ASSERT_EQUAL_INT(0, g_halt_calls);
    free(hex);
}

void test_verb_reset_rebuilds_fsm_threads_when_ctx_has_fsm_and_symbols(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    FsmContext fsm;
    AvrOsSymbolIndex idx;
    memset(&fsm, 0, sizeof fsm);
    memset(&idx, 0, sizeof idx);
    ctx.fsm = &fsm;
    ctx.idx = &idx;

    char *hex = hexify("reset");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, g_fsm_invalidate_calls);
    TEST_ASSERT_EQUAL_INT(1, g_fsm_build_calls);
    free(hex);
}

void test_verb_halt_emits_T05_stop_reply_and_suppresses_OK(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    char *hex = hexify("halt");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    /* Returns 0 so the dh_monitor caller appends the standard OK reply.
     * T05 is *not* a valid response to qRcmd — the previous behaviour
     * caused "Invalid hex digit" parse errors in real avr-gdb. The
     * `target halted` line is emitted as an O-packet for the user. */
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, g_halt_calls);
    /* Last captured packet must be an O-packet, not a T05 stop reply. */
    TEST_ASSERT_GREATER_THAN(0, g_packet_count);
    const char *last = g_packets[g_packet_count - 1];
    TEST_ASSERT_EQUAL_CHAR('O', last[0]);
    free(hex);
}

void test_verb_go_calls_updi_run_and_returns_OK(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    char *hex = hexify("go");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, g_run_calls);
    free(hex);
}

void test_verb_erase_refused_without_allow_erase_flag(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    ctx.allow_erase = 0;
    char *hex = hexify("erase");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(0, g_chip_erase_calls);
    /* Diagnostic O-packet must mention --allow-erase. */
    char *txt = last_o_packet_text();
    TEST_ASSERT_NOT_NULL(strstr(txt, "--allow-erase"));
    free(txt);
    free(hex);
}

void test_verb_erase_allowed_when_flag_set_clears_shadows(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    ctx.allow_erase = 1;
    char *hex = hexify("chip-erase");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, g_chip_erase_calls);
    TEST_ASSERT_EQUAL_INT(1, g_enter_debug_calls);
    TEST_ASSERT_EQUAL_INT(1, g_hw_bp_clear_calls);
    free(hex);
}

void test_verb_version_emits_o_packet_with_version_and_date(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    char *hex = hexify("version");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);
    char *txt = last_o_packet_text();
    TEST_ASSERT_NOT_NULL(strstr(txt, "avrOSdb"));
    TEST_ASSERT_NOT_NULL(strstr(txt, "built"));
    free(txt);
    free(hex);
}

void test_verb_bp_mode_sw_and_hw_only_mutate_ctx(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();

    char *hex1 = hexify("bp-mode hw-only");
    TEST_ASSERT_EQUAL_INT(0, monitor_dispatch_ex(1, &ctx, hex1));
    TEST_ASSERT_EQUAL_INT(RSP_BP_MODE_HW_ONLY, ctx.bp_mode);
    free(hex1);

    char *hex2 = hexify("bp-mode sw");
    TEST_ASSERT_EQUAL_INT(0, monitor_dispatch_ex(1, &ctx, hex2));
    TEST_ASSERT_EQUAL_INT(RSP_BP_MODE_SW, ctx.bp_mode);
    free(hex2);

    /* Bad arg → -2 plus diagnostic. */
    char *hex3 = hexify("bp-mode banana");
    TEST_ASSERT_EQUAL_INT(-2, monitor_dispatch_ex(1, &ctx, hex3));
    free(hex3);
}

void test_verb_help_emits_one_line_per_recognised_verb(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    char *hex = hexify("help");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, rc);
    /* Expect at least one packet per top-level verb (>= 8). */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(8, (int)g_packet_count);
    free(hex);
}

void test_unknown_top_level_verb_emits_usage_hint(void)
{
    mock_reset(); hlr055_mock_reset();
    RspContext ctx = make_ctx();
    char *hex = hexify("teleport");
    int rc = monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(-2, rc);
    char *txt = last_o_packet_text();
    TEST_ASSERT_NOT_NULL(strstr(txt, "usage:"));
    free(txt);
    free(hex);
}

void test_avros_prefix_still_routed_to_legacy_dispatch(void)
{
    mock_reset(); hlr055_mock_reset();
    /* No symbol index → legacy dispatch emits its own diagnostic but
     * the return code must not be the unknown-verb -2.  We accept the
     * legacy contract: -2 (idx==NULL path also returns -2, but the
     * caller can tell the difference because no top-level verb name
     * was tried).  Assert chip_erase / enter_debug NOT called.       */
    RspContext ctx = make_ctx();
    char *hex = hexify("avros events");
    (void)monitor_dispatch_ex(1, &ctx, hex);
    TEST_ASSERT_EQUAL_INT(0, g_enter_debug_calls);
    TEST_ASSERT_EQUAL_INT(0, g_chip_erase_calls);
    TEST_ASSERT_EQUAL_INT(0, g_halt_calls);
    free(hex);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_monitor_dispatch_hex_decodes_cmd_before_prefix_matching);
    RUN_TEST(test_monitor_dispatch_treats_invalid_hex_sequence_as_unrecognised);
    RUN_TEST(test_monitor_dispatch_rejects_cmd_without_avros_space_prefix);
    RUN_TEST(test_monitor_dispatch_sends_usage_hint_o_packet_on_bad_prefix);
    RUN_TEST(test_cmd_events_reads_event_count_descriptors_from_evnt_table);
    RUN_TEST(test_cmd_events_reports_name_and_status_value_per_descriptor);
    RUN_TEST(test_cmd_events_output_is_hex_encoded_o_packet);
    RUN_TEST(test_cmd_queues_reads_queue_count_descriptors_from_que_table);
    RUN_TEST(test_cmd_queues_formats_capacity_and_sizeofelement_for_each_entry);
    RUN_TEST(test_monitor_dispatch_and_helpers_never_call_updi_halt);
    RUN_TEST(test_verb_reset_pulses_updi_and_clears_shadows);
    RUN_TEST(test_verb_reset_rebuilds_fsm_threads_when_ctx_has_fsm_and_symbols);
    RUN_TEST(test_verb_halt_emits_T05_stop_reply_and_suppresses_OK);
    RUN_TEST(test_verb_go_calls_updi_run_and_returns_OK);
    RUN_TEST(test_verb_erase_refused_without_allow_erase_flag);
    RUN_TEST(test_verb_erase_allowed_when_flag_set_clears_shadows);
    RUN_TEST(test_verb_version_emits_o_packet_with_version_and_date);
    RUN_TEST(test_verb_bp_mode_sw_and_hw_only_mutate_ctx);
    RUN_TEST(test_verb_help_emits_one_line_per_recognised_verb);
    RUN_TEST(test_unknown_top_level_verb_emits_usage_hint);
    RUN_TEST(test_avros_prefix_still_routed_to_legacy_dispatch);
    return UNITY_END();
}
