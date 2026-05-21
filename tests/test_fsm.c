/* tests/test_fsm.c — Unity tests for src/fsm_mapper.c (Phase 3)
 *
 * Validates fsm_build_thread_list / fsm_get_registers / fsm_invalidate
 * / fsm_get_active_thread against the real avrOS FSM_TABLE layout:
 *
 *   fsmStateMachineDescr_t  (9 bytes, FLASH):
 *     +0 name_ptr (LE u16)   +2 sm_ptr (LE u16)
 *     +4 handler  (LE u16)   +6 priority (u8)  +7 instance (LE u16)
 *
 *   fsmStateMachine_t  (SRAM):
 *     +9 currState  (LE u16)
 *
 *   currStateMachine (SRAM, LE u16)
 *
 * UPDI access is mocked via __wrap_updi_mem_read with a "backing store"
 * of address-range → bytes regions, plus a call log so tests can assert
 * how many UPDI reads occurred and at which addresses.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "unity.h"
#include "fsm_mapper.h"
#include "updi.h"   /* flash_to_updi, sram_to_updi */

/* ── Mock UPDI backing-store ─────────────────────────────────────────── */
#define MAX_REGIONS 96
#define MAX_CALLS   2048

typedef struct {
    uint32_t       start;
    uint32_t       len;
    const uint8_t *data;
} MockRegion;

typedef struct {
    uint32_t addr;
    size_t   len;
    int      result;  /* return value of the mock for this call */
} MockCall;

static MockRegion g_regions[MAX_REGIONS];
static int        g_region_count;
static MockCall   g_calls[MAX_CALLS];
static int        g_call_count;
static int        g_force_fail_at; /* 1-based; 0 = never */

static void mock_reset(void)
{
    memset(g_regions, 0, sizeof(g_regions));
    memset(g_calls,   0, sizeof(g_calls));
    g_region_count  = 0;
    g_call_count    = 0;
    g_force_fail_at = 0;
}

static void mock_add_region(uint32_t start, const uint8_t *data, uint32_t len)
{
    TEST_ASSERT_LESS_THAN(MAX_REGIONS, g_region_count);
    g_regions[g_region_count++] = (MockRegion){ start, len, data };
}

int __wrap_updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)fd;
    int idx = g_call_count;
    if (idx < MAX_CALLS) {
        g_calls[idx].addr = addr;
        g_calls[idx].len  = len;
    }
    g_call_count++;

    if (g_force_fail_at > 0 && g_call_count == g_force_fail_at) {
        if (idx < MAX_CALLS) g_calls[idx].result = -1;
        return -1;
    }

    /* Find a region containing [addr, addr+len). */
    for (int i = 0; i < g_region_count; i++) {
        const MockRegion *r = &g_regions[i];
        if (addr >= r->start && addr + len <= r->start + r->len) {
            memcpy(buf, r->data + (addr - r->start), len);
            if (idx < MAX_CALLS) g_calls[idx].result = 0;
            return 0;
        }
    }
    /* Address with no backing region → zero-fill (graceful default). */
    memset(buf, 0, len);
    if (idx < MAX_CALLS) g_calls[idx].result = 0;
    return 0;
}

void setUp(void)    { mock_reset(); }
void tearDown(void) { mock_reset(); }

/* ── Fixture: one canonical 2-entry FSM_TABLE with active = entry #2 ── */
#define FSM_TABLE_ADDR    0x1000U
#define CURRENT_FSM_ADDR  0x802100U

/* Entry 1: name="t1", sm_ptr=0x4000, state=0x00AA */
/* Entry 2: name="t2", sm_ptr=0x4100, state=0xABCD  (active) */
static const uint8_t fsm_table_2entries[2 * 9] = {
    /* descr[0] @0x1000: name=0x6000, sm=0x4000, handler=0, prio=0, inst=0 */
    0x00, 0x60,  0x00, 0x40,  0x00, 0x00,  0x00,  0x00, 0x00,
    /* descr[1] @0x1009: name=0x6010, sm=0x4100, handler=0, prio=0, inst=0 */
    0x10, 0x60,  0x00, 0x41,  0x00, 0x00,  0x00,  0x00, 0x00,
};
/* SRAM @0x4000: 0..8 padding, +9 = currState LE = 0xAA, 0x00 */
static const uint8_t sm1_sram[11] = {
    0,0,0,0,0,0,0,0,0, 0xAA, 0x00
};
static const uint8_t sm2_sram[11] = {
    0,0,0,0,0,0,0,0,0, 0xCD, 0xAB
};
/* currStateMachine SRAM: LE 0x4100 = entry-2 is active */
static const uint8_t current_active2[2] = { 0x00, 0x41 };
/* FLASH name strings */
static const uint8_t name1[] = { 't','1', 0 };
static const uint8_t name2[] = { 't','2', 0 };

static void install_2entry_fixture(AvrOsSymbolIndex *idx)
{
    memset(idx, 0, sizeof(*idx));
    idx->fsm_table_addr   = FSM_TABLE_ADDR;
    idx->fsm_table_count  = 2;
    idx->current_fsm_addr = CURRENT_FSM_ADDR;

    /* FLASH-side data (FSM_TABLE, name strings) is fetched via UPDI's
     * flash mirror (flash_to_updi).  SRAM-side data (sm_ptr targets,
     * currStateMachine) uses sram_to_updi to strip the GDB-AVR data
     * flag. */
    mock_add_region(flash_to_updi(FSM_TABLE_ADDR),
                    fsm_table_2entries, sizeof(fsm_table_2entries));
    mock_add_region(0x4000,           sm1_sram,           sizeof(sm1_sram));
    mock_add_region(0x4100,           sm2_sram,           sizeof(sm2_sram));
    mock_add_region(flash_to_updi(0x6000), name1,         sizeof(name1));
    mock_add_region(flash_to_updi(0x6010), name2,         sizeof(name2));
    mock_add_region(sram_to_updi(CURRENT_FSM_ADDR),
                    current_active2, sizeof(current_active2));
}

/* ──────────────────────────────────────────────────────────────────────
 * STP-aligned tests (Phase 3, LLR-FSM-01 … LLR-FSM-06)
 * ────────────────────────────────────────────────────────────────────── */

/* 1) reads FSM_TABLE from FLASH via UPDI */
void test_fsm_build_thread_list_reads_fsm_table_from_flash_via_updi(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);

    FsmContext ctx;
    int n = fsm_build_thread_list(&ctx, &idx, /*fd*/ 7);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_TRUE(ctx.valid);

    /* At least one UPDI read hit the FSM_TABLE address (in UPDI flash
     * mirror form). */
    uint32_t fsm_updi = flash_to_updi(FSM_TABLE_ADDR);
    int saw_fsm_read = 0;
    for (int i = 0; i < g_call_count; i++)
        if (g_calls[i].addr >= fsm_updi
            && g_calls[i].addr < fsm_updi + 2 * 9)
            saw_fsm_read = 1;
    TEST_ASSERT_TRUE_MESSAGE(saw_fsm_read,
        "expected at least one updi_mem_read covering FSM_TABLE");
}

/* 2) returns -1 on UPDI failure */
void test_fsm_build_thread_list_returns_minus1_on_updi_failure(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);
    g_force_fail_at = 1; /* fail the first read (currStateMachine) */

    FsmContext ctx;
    int n = fsm_build_thread_list(&ctx, &idx, /*fd*/ 7);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* 3) assigns 1-based thread IDs in order */
void test_fsm_build_thread_list_assigns_1_based_thread_ids_in_order(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);

    FsmContext ctx;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&ctx, &idx, 7));
    TEST_ASSERT_EQUAL_INT(1, ctx.threads[0].gdb_id);
    TEST_ASSERT_EQUAL_INT(2, ctx.threads[1].gdb_id);
}

/* 4) thread IDs stable across calls */
void test_fsm_build_thread_list_thread_ids_stable_across_calls(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);

    FsmContext a, b;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&a, &idx, 7));
    /* Reset call log but keep regions installed. */
    g_call_count = 0;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&b, &idx, 7));

    TEST_ASSERT_EQUAL_INT(a.threads[0].gdb_id, b.threads[0].gdb_id);
    TEST_ASSERT_EQUAL_INT(a.threads[1].gdb_id, b.threads[1].gdb_id);
    TEST_ASSERT_EQUAL_UINT32(a.threads[0].state_fn, b.threads[0].state_fn);
    TEST_ASSERT_EQUAL_UINT32(a.threads[1].state_fn, b.threads[1].state_fn);
}

/* 5) sets active thread from currStateMachine ptr */
void test_fsm_build_thread_list_sets_active_thread_from_current_fsm_ptr(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);

    FsmContext ctx;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&ctx, &idx, 7));
    TEST_ASSERT_FALSE(ctx.threads[0].is_active);
    TEST_ASSERT_TRUE (ctx.threads[1].is_active);
    TEST_ASSERT_EQUAL_INT(2, ctx.active_id);
}

/* 6) active_id = 0 when currStateMachine matches no entry */
void test_fsm_build_thread_list_sets_active_id_0_when_no_entry_matches(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);
    /* Replace the currStateMachine region with one pointing nowhere. */
    static const uint8_t curr_nomatch[2] = { 0xFF, 0x7F };
    g_regions[5].data = curr_nomatch;

    FsmContext ctx;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&ctx, &idx, 7));
    TEST_ASSERT_FALSE(ctx.threads[0].is_active);
    TEST_ASSERT_FALSE(ctx.threads[1].is_active);
    TEST_ASSERT_EQUAL_INT(0, ctx.active_id);
}

/* 7) PC bytes at hex positions [70..77] = LE(state_fn) */
void test_fsm_get_registers_places_state_fn_as_pc_at_hex_positions_70_77(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);

    FsmContext ctx;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&ctx, &idx, 7));
    TEST_ASSERT_EQUAL_UINT32(0xABCDU, ctx.threads[1].state_fn);

    char reg[80];
    memset(reg, 'X', sizeof(reg));
    TEST_ASSERT_EQUAL_INT(0, fsm_get_registers(&ctx, 2, reg));

    /* PC = 0xABCD → LE bytes CD AB 00 00 → hex "cdab00000" sequence */
    TEST_ASSERT_EQUAL_STRING_LEN("cdab0000", &reg[70], 8);
    TEST_ASSERT_EQUAL_CHAR('\0', reg[78]);
}

/* 8) non-active thread: R0..R31 & SREG/SPL/SPH all zero */
void test_fsm_get_registers_non_active_r0_r31_sreg_spl_sph_all_zero(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);

    FsmContext ctx;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&ctx, &idx, 7));

    char reg[80];
    TEST_ASSERT_EQUAL_INT(0, fsm_get_registers(&ctx, 1, reg)); /* non-active */

    /* Positions [0..69] must all be '0' chars. */
    for (int i = 0; i < 70; i++)
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('0', reg[i], "non-active byte must be 0");
}

/* 9) active thread: reads live SREG/SPL/SPH via UPDI */
void test_fsm_get_registers_active_thread_reads_live_sreg_spl_sph(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);
    /* Install I/O-space region with SPL=0x12, SPH=0x34, SREG=0xC0
     * at byte address 0x3D (SPL),0x3E (SPH),0x3F (SREG). */
    static const uint8_t io_state[3] = { 0x12, 0x34, 0xC0 };
    mock_add_region(0x3D, io_state, sizeof(io_state));

    FsmContext ctx;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&ctx, &idx, 7));

    /* Snapshot call count before fsm_get_registers. */
    int before = g_call_count;
    char reg[80];
    TEST_ASSERT_EQUAL_INT(0, fsm_get_registers(&ctx, 2, reg)); /* active */
    int after = g_call_count;
    TEST_ASSERT_GREATER_THAN_MESSAGE(before, after,
        "active fsm_get_registers must issue >=1 updi_mem_read");

    /* SREG @ hex pos 64..65 = "c0", SPL @ 66..67 = "12", SPH @ 68..69 = "34". */
    TEST_ASSERT_EQUAL_STRING_LEN("c0", &reg[64], 2);
    TEST_ASSERT_EQUAL_STRING_LEN("12", &reg[66], 2);
    TEST_ASSERT_EQUAL_STRING_LEN("34", &reg[68], 2);
}

/* 10) caps at 32 entries and logs warning */
void test_fsm_build_thread_list_caps_at_32_entries_and_logs_warning(void)
{
    /* Build a synthetic 64-entry table, each entry sm_ptr=0x4000+i. */
    static uint8_t big_table[64 * 9];
    static uint8_t sm_pages[64][11];
    memset(big_table, 0, sizeof(big_table));
    memset(sm_pages,  0, sizeof(sm_pages));
    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    idx.fsm_table_addr   = FSM_TABLE_ADDR;
    idx.fsm_table_count  = 64;
    idx.current_fsm_addr = 0; /* skip currStateMachine read */

    for (int i = 0; i < 64; i++) {
        uint16_t sm = (uint16_t)(0x4000 + i * 0x10);
        big_table[i * 9 + 2] = (uint8_t)(sm      );
        big_table[i * 9 + 3] = (uint8_t)(sm >>  8);
        mock_add_region(sm, sm_pages[i], sizeof(sm_pages[i]));
    }
    mock_add_region(flash_to_updi(FSM_TABLE_ADDR),
                    big_table, sizeof(big_table));

    FsmContext ctx;
    int n = fsm_build_thread_list(&ctx, &idx, 7);
    TEST_ASSERT_EQUAL_INT(FSM_MAX_THREADS, n);
    TEST_ASSERT_EQUAL_INT(FSM_MAX_THREADS, ctx.thread_count);
}

/* 11) non-active fsm_get_registers issues ZERO updi_mem_read calls */
void test_fsm_get_registers_non_active_no_updi_read_of_stack(void)
{
    AvrOsSymbolIndex idx;
    install_2entry_fixture(&idx);

    FsmContext ctx;
    TEST_ASSERT_EQUAL_INT(2, fsm_build_thread_list(&ctx, &idx, 7));

    int before = g_call_count;
    char reg[80];
    TEST_ASSERT_EQUAL_INT(0, fsm_get_registers(&ctx, 1, reg)); /* non-active */
    int after = g_call_count;
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, after,
        "non-active fsm_get_registers must issue 0 updi_mem_read");
}

/* ── Runner ──────────────────────────────────────────────────────────── */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fsm_build_thread_list_reads_fsm_table_from_flash_via_updi);
    RUN_TEST(test_fsm_build_thread_list_returns_minus1_on_updi_failure);
    RUN_TEST(test_fsm_build_thread_list_assigns_1_based_thread_ids_in_order);
    RUN_TEST(test_fsm_build_thread_list_thread_ids_stable_across_calls);
    RUN_TEST(test_fsm_build_thread_list_sets_active_thread_from_current_fsm_ptr);
    RUN_TEST(test_fsm_build_thread_list_sets_active_id_0_when_no_entry_matches);
    RUN_TEST(test_fsm_get_registers_places_state_fn_as_pc_at_hex_positions_70_77);
    RUN_TEST(test_fsm_get_registers_non_active_r0_r31_sreg_spl_sph_all_zero);
    RUN_TEST(test_fsm_get_registers_active_thread_reads_live_sreg_spl_sph);
    RUN_TEST(test_fsm_build_thread_list_caps_at_32_entries_and_logs_warning);
    RUN_TEST(test_fsm_get_registers_non_active_no_updi_read_of_stack);
    return UNITY_END();
}
