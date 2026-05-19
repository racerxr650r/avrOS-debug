/* tests/test_elf.c — Unit tests for src/elf_parser.c (Phase 1, Phase 3 aligned) */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "elf_parser.h"

/* ── Fixture paths (relative to repo root, where make test runs from) ─── */
#define FIXTURE_DIR     "build/fixtures"
#define FIXTURE_FULL    FIXTURE_DIR "/avros_full.elf"
#define FIXTURE_PARTIAL FIXTURE_DIR "/avros_partial.elf"

/* ── malloc wrap infrastructure ──────────────────────────────────────────
 * Enabled by -Wl,--wrap,malloc in TEST_WRAP_test_elf.
 * Set g_malloc_fail_at to N > 0 to return NULL on the Nth call.
 * Reset to 0 to restore normal behaviour.
 */
extern void *__real_malloc(size_t sz);

static int g_malloc_fail_at    = 0;
static int g_malloc_call_count = 0;

void *__wrap_malloc(size_t sz)
{
    ++g_malloc_call_count;
    if (g_malloc_fail_at > 0 && g_malloc_call_count >= g_malloc_fail_at)
        return NULL;
    return __real_malloc(sz);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */
static void reset_malloc_hook(void)
{
    g_malloc_fail_at    = 0;
    g_malloc_call_count = 0;
}

void setUp(void)    { reset_malloc_hook(); }
void tearDown(void) { reset_malloc_hook(); }

/* ── Helper: write a minimal ELF32 header to a temp file ─────────────── */
static int write_minimal_elf32(const char *path, uint16_t machine)
{
    unsigned char buf[52]; /* sizeof(Elf32_Ehdr) */
    memset(buf, 0, sizeof(buf));
    /* Magic */
    buf[0] = 0x7f; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1;    /* ELFCLASS32 */
    buf[5] = 1;    /* ELFDATA2LSB */
    buf[6] = 1;    /* EV_CURRENT */
    /* e_machine at bytes 18-19 (little-endian) */
    buf[18] = (unsigned char)(machine & 0xFFu);
    buf[19] = (unsigned char)((machine >> 8) & 0xFFu);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    ssize_t written = write(fd, buf, sizeof(buf));
    close(fd);
    return (written == (ssize_t)sizeof(buf)) ? 0 : -1;
}

/* ── Test 1: valid AVR ELF32 accepted ────────────────────────────────── */
void test_elf_open_accepts_valid_avr_elf32_binary(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    int r = elf_open(FIXTURE_FULL, &ctx);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_GREATER_THAN(0, (int)ctx.sym_count);
    elf_close(&ctx);
}

/* ── Test 2: bad magic rejected ──────────────────────────────────────── */
void test_elf_open_returns_minus1_on_invalid_elf_magic(void)
{
    char tmppath[] = "/tmp/aod_bad_magic_XXXXXX";
    int fd = mkstemp(tmppath);
    TEST_ASSERT_NOT_EQUAL(-1, fd);
    unsigned char bad[52];
    memset(bad, 0, sizeof(bad));
    bad[0] = 0xDE; bad[1] = 0xAD; bad[2] = 0xBE; bad[3] = 0xEF;
    write(fd, bad, sizeof(bad));
    close(fd);

    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    int r = elf_open(tmppath, &ctx);
    unlink(tmppath);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

/* ── Test 3: wrong machine type rejected ─────────────────────────────── */
void test_elf_open_returns_minus1_on_wrong_machine_type(void)
{
    char tmppath[] = "/tmp/aod_wrong_mach_XXXXXX";
    int fd = mkstemp(tmppath);
    TEST_ASSERT_NOT_EQUAL(-1, fd);
    close(fd);
    /* EM_386 = 3, not EM_AVR */
    TEST_ASSERT_EQUAL_INT(0, write_minimal_elf32(tmppath, 3));

    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    int r = elf_open(tmppath, &ctx);
    unlink(tmppath);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

/* ── Test 4: symtab and strtab loaded into heap ──────────────────────── */
void test_elf_open_loads_symtab_and_strtab_into_heap_buffers(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.symtab);
    TEST_ASSERT_NOT_NULL(ctx.strtab);
    TEST_ASSERT_GREATER_THAN(0, (int)ctx.sym_count);
    TEST_ASSERT_GREATER_THAN(0, (int)ctx.strtab_size);
    elf_close(&ctx);
}

/* ── Test 5: malloc failure → partial allocs freed, -1 returned ─────── */
void test_elf_open_frees_partial_allocs_and_returns_minus1_on_malloc_failure(void)
{
    /* Reset counter, then fail on the 2nd malloc (strtab allocation) */
    g_malloc_call_count = 0;
    g_malloc_fail_at    = 2;

    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    int r = elf_open(FIXTURE_FULL, &ctx);

    g_malloc_fail_at = 0; /* restore */

    TEST_ASSERT_EQUAL_INT(-1, r);
    /* First allocation (symtab) must have been freed */
    TEST_ASSERT_NULL(ctx.symtab);
    TEST_ASSERT_NULL(ctx.strtab);
    /* ASAN (make ASAN=1 test) will catch any leak */
}

/* ── Test 6: single linear scan with known in-memory symbol table ────── *
 * Hand-craft a minimal ctx with 8 symbols spread across the table and    *
 * verify that elf_find_avros_tables populates all 7 sentinel fields with *
 * the exact expected values. The implementation must examine every       *
 * entry exactly once (O(n)) to produce correct results here.             *
 *                                                                         *
 * Strtab layout (offsets, each entry includes its NUL terminator):       *
 *  [0]   '\0'                                                             *
 *  [1]   "__start_FSM_TABLE\0"    (18 bytes → next at  19)               *
 *  [19]  "__stop_FSM_TABLE\0"     (17 bytes → next at  36)               *
 *  [36]  "__start_QUE_TABLE\0"    (18 bytes → next at  54)               *
 *  [54]  "__stop_QUE_TABLE\0"     (17 bytes → next at  71)               *
 *  [71]  "__start_EVNT_TABLE\0"   (19 bytes → next at  90)               *
 *  [90]  "__stop_EVNT_TABLE\0"    (18 bytes → next at 108)               *
 *  [108] "currStateMachine\0"     (17 bytes → total   125)               *
 * ─────────────────────────────────────────────────────────────────────── */
void test_elf_find_avros_tables_performs_single_linear_scan(void)
{
    static const char strtab[] =
        "\0"
        "__start_FSM_TABLE\0"
        "__stop_FSM_TABLE\0"
        "__start_QUE_TABLE\0"
        "__stop_QUE_TABLE\0"
        "__start_EVNT_TABLE\0"
        "__stop_EVNT_TABLE\0"
        "currStateMachine\0";

    /* Sym offsets into strtab (pre-computed from the layout above) */
    enum {
        OFF_FSM_S    =   1, OFF_FSM_E    =  19,
        OFF_Q_S      =  36, OFF_Q_E      =  54,
        OFF_EVT_S    =  71, OFF_EVT_E    =  90,
        OFF_CURR     = 108
    };

    Elf32_Sym syms[8];
    memset(syms, 0, sizeof(syms));
    /* sym[0]: anonymous → skip */
    syms[0].st_name  = 0;
    syms[0].st_shndx = 1;
    syms[0].st_value = 0;

    /* FSM:   stop - start = 0x12 = 18 → 18 / 9  = 2 entries */
    syms[1].st_name = OFF_FSM_S; syms[1].st_shndx = 1; syms[1].st_value = 0x1000U;
    syms[2].st_name = OFF_FSM_E; syms[2].st_shndx = 1; syms[2].st_value = 0x1012U;
    /* QUE:   stop - start = 0x14 = 20 → 20 / 10 = 2 entries */
    syms[3].st_name = OFF_Q_S;   syms[3].st_shndx = 1; syms[3].st_value = 0x2000U;
    syms[4].st_name = OFF_Q_E;   syms[4].st_shndx = 1; syms[4].st_value = 0x2014U;
    /* EVNT:  stop - start = 0x10 = 16 → 16 / 4  = 4 entries */
    syms[5].st_name = OFF_EVT_S; syms[5].st_shndx = 1; syms[5].st_value = 0x3000U;
    syms[6].st_name = OFF_EVT_E; syms[6].st_shndx = 1; syms[6].st_value = 0x3010U;
    /* currStateMachine: SRAM raw VMA */
    syms[7].st_name = OFF_CURR;  syms[7].st_shndx = 1; syms[7].st_value = 0x00802100U;

    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.symtab      = syms;
    ctx.sym_count   = 8;
    ctx.strtab      = (char *)strtab;
    ctx.strtab_size = sizeof(strtab);
    ctx.flash_base  = 0;
    ctx.fd          = -1;

    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    int r = elf_find_avros_tables(&ctx, &idx);

    TEST_ASSERT_EQUAL_INT(0, r);

    /* flash_base=0, so elf_flash_addr(vma) = vma/2 */
    TEST_ASSERT_EQUAL_UINT32(0x0800U,     idx.fsm_table_addr);
    TEST_ASSERT_EQUAL_UINT8 (2U,          idx.fsm_table_count);
    TEST_ASSERT_EQUAL_UINT32(0x1000U,     idx.queue_table_addr);
    TEST_ASSERT_EQUAL_UINT8 (2U,          idx.queue_count);
    TEST_ASSERT_EQUAL_UINT32(0x1800U,     idx.event_table_addr);
    TEST_ASSERT_EQUAL_UINT8 (4U,          idx.event_count);
    TEST_ASSERT_EQUAL_UINT32(0x00802100U, idx.current_fsm_addr);

    /* Do NOT call elf_close — symtab/strtab are stack/static, not heap */
    ctx.symtab  = NULL;
    ctx.strtab  = NULL;
}

/* ── Test 7: all 7 sentinel fields non-zero with full fixture ────────── */
void test_elf_find_avros_tables_populates_all_7_avros_sentinel_fields(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    TEST_ASSERT_EQUAL_INT(0, elf_find_avros_tables(&ctx, &idx));

    TEST_ASSERT_NOT_EQUAL(0U, idx.fsm_table_addr);
    TEST_ASSERT_NOT_EQUAL(0U, idx.fsm_table_count);
    TEST_ASSERT_NOT_EQUAL(0U, idx.queue_table_addr);
    TEST_ASSERT_NOT_EQUAL(0U, idx.queue_count);
    TEST_ASSERT_NOT_EQUAL(0U, idx.event_table_addr);
    TEST_ASSERT_NOT_EQUAL(0U, idx.event_count);
    TEST_ASSERT_NOT_EQUAL(0U, idx.current_fsm_addr);

    elf_close(&ctx);
}

/* ── Test 8: elf_flash_addr formula (vma - flash_base) / 2 ─────────── */
void test_elf_flash_addr_applies_vma_minus_base_over_2_formula(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.flash_base = 0x1000U;
    ctx.fd         = -1;

    TEST_ASSERT_EQUAL_UINT32(0x0800U, elf_flash_addr(&ctx, 0x2000U));
    TEST_ASSERT_EQUAL_UINT32(0x0000U, elf_flash_addr(&ctx, 0x1000U));
    TEST_ASSERT_EQUAL_UINT32(0x0001U, elf_flash_addr(&ctx, 0x1002U));
}

/* ── Test 9: FLASH-resident idx fields computed via elf_flash_addr ───── */
void test_elf_flash_addr_all_avros_symbol_addresses_use_word_formula(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    TEST_ASSERT_EQUAL_INT(0, elf_find_avros_tables(&ctx, &idx));

    /* Scan the loaded symtab to find the raw _start VMAs */
    uint32_t fsm_vma = 0, queue_vma = 0, event_vma = 0, curr_vma = 0;
    for (size_t i = 0; i < ctx.sym_count; i++) {
        const Elf32_Sym *s = &ctx.symtab[i];
        if (s->st_shndx == SHN_UNDEF || s->st_name == 0) continue;
        if ((size_t)s->st_name >= ctx.strtab_size)        continue;
        const char *nm = ctx.strtab + s->st_name;
        if      (!strcmp(nm, "__start_FSM_TABLE"))  fsm_vma   = s->st_value;
        else if (!strcmp(nm, "__start_QUE_TABLE"))  queue_vma = s->st_value;
        else if (!strcmp(nm, "__start_EVNT_TABLE")) event_vma = s->st_value;
        else if (!strcmp(nm, "currStateMachine"))   curr_vma  = s->st_value;
    }

    /* FLASH-resident: must equal (vma - flash_base) / 2 */
    TEST_ASSERT_NOT_EQUAL(0U, fsm_vma);
    TEST_ASSERT_EQUAL_UINT32(elf_flash_addr(&ctx, fsm_vma),   idx.fsm_table_addr);
    TEST_ASSERT_EQUAL_UINT32(elf_flash_addr(&ctx, queue_vma), idx.queue_table_addr);
    TEST_ASSERT_EQUAL_UINT32(elf_flash_addr(&ctx, event_vma), idx.event_table_addr);

    /* SRAM-resident: must be the raw VMA, not a word address */
    TEST_ASSERT_EQUAL_UINT32(curr_vma, idx.current_fsm_addr);

    elf_close(&ctx);
}

/* ── Test 10: partial fixture → returns 0, not -1 ────────────────────── */
void test_elf_find_avros_tables_returns_0_on_partial_symbol_match(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_PARTIAL, &ctx));

    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    int r = elf_find_avros_tables(&ctx, &idx);
    TEST_ASSERT_EQUAL_INT(0, r); /* must not return -1 */

    elf_close(&ctx);
}

/* ── Test 11: absent symbols stay zero-initialised ───────────────────── */
void test_elf_find_avros_tables_zero_initialises_absent_symbol_fields(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_PARTIAL, &ctx));

    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    TEST_ASSERT_EQUAL_INT(0, elf_find_avros_tables(&ctx, &idx));

    /* Partial fixture has only FSM_TABLE sentinels + currStateMachine. */
    /* Queue and event table fields must remain zero-initialised.       */
    TEST_ASSERT_EQUAL_UINT32(0U, idx.queue_table_addr);
    TEST_ASSERT_EQUAL_UINT8 (0U, idx.queue_count);
    TEST_ASSERT_EQUAL_UINT32(0U, idx.event_table_addr);
    TEST_ASSERT_EQUAL_UINT8 (0U, idx.event_count);

    elf_close(&ctx);
}

/* ── Test 12: elf_close frees heap and closes fd ─────────────────────── */
void test_elf_close_frees_symtab_strtab_and_closes_fd(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    int saved_fd = ctx.fd;
    TEST_ASSERT_NOT_EQUAL(-1, saved_fd);
    TEST_ASSERT_NOT_NULL(ctx.symtab);
    TEST_ASSERT_NOT_NULL(ctx.strtab);

    elf_close(&ctx);

    TEST_ASSERT_EQUAL_INT(-1, ctx.fd);
    TEST_ASSERT_NULL(ctx.symtab);
    TEST_ASSERT_NULL(ctx.strtab);

    /* Verify the OS fd is actually closed */
    TEST_ASSERT_EQUAL_INT(-1, fcntl(saved_fd, F_GETFD));
    TEST_ASSERT_EQUAL_INT(EBADF, errno);
    /* ASAN (make ASAN=1 test) will catch any heap leak */
}

/* ── Test 13: elf_close safe on partially initialised context ────────── */
void test_elf_close_safe_on_partially_initialised_context(void)
{
    /* symtab = NULL, strtab allocated, fd = -1 */
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.symtab      = NULL;
    ctx.strtab      = malloc(16);
    TEST_ASSERT_NOT_NULL(ctx.strtab);
    ctx.strtab_size = 16;
    ctx.fd          = -1;

    /* Must not crash, double-free, or leak */
    elf_close(&ctx);

    TEST_ASSERT_NULL(ctx.symtab);
    TEST_ASSERT_NULL(ctx.strtab);
    TEST_ASSERT_EQUAL_INT(-1, ctx.fd);
}

/* ── Test 14: flash_base and sram_base populated from PT_LOAD segments ─ */
void test_elf_open_sets_flash_base_and_sram_base_from_pt_load_segments(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    /* avros_full.elf is built for AVR128DA28:
     *   first  PT_LOAD → FLASH  VMA 0x00000000
     *   second PT_LOAD → SRAM   VMA 0x00804000 */
    TEST_ASSERT_EQUAL_UINT32(0x00000000UL, ctx.flash_base);
    TEST_ASSERT_EQUAL_UINT32(0x00804000UL, ctx.sram_base);
    TEST_ASSERT_GREATER_THAN(0, (int)ctx.flash_size);

    elf_close(&ctx);
}

/* ── Test: deviceinfo note → device_name populated ─────────────────── */
void test_elf_open_extracts_device_name_from_deviceinfo_note(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));
    /* All AVR fixtures are built for AVR128DA28. */
    TEST_ASSERT_EQUAL_STRING("avr128da28", ctx.device_name);
    elf_close(&ctx);
}

/* ── Test: absent deviceinfo note → device_name stays empty ────────── */
void test_elf_open_leaves_device_name_empty_when_note_absent(void)
{
    /* Build a synthetic ELF32 AVR header with PT_LOAD + SYMTAB but no
     * .note.gnu.avr.deviceinfo section; assert ctx.device_name stays
     * the empty string we expect on missing-note inputs.              */
    char tmppath[] = "/tmp/aod_no_note_XXXXXX";
    int fd = mkstemp(tmppath);
    TEST_ASSERT_NOT_EQUAL(-1, fd);
    close(fd);
    /* Reuse the avros_full fixture but defeat the note: copy the file,
     * then zero its SHT_NOTE bytes so the scan can't find "avr…".     */
    int rfd = open(FIXTURE_FULL, O_RDONLY);
    TEST_ASSERT_NOT_EQUAL(-1, rfd);
    int wfd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT_NOT_EQUAL(-1, wfd);
    char io[4096];
    ssize_t n;
    while ((n = read(rfd, io, sizeof io)) > 0) {
        TEST_ASSERT_EQUAL_INT(n, write(wfd, io, (size_t)n));
    }
    close(rfd);
    close(wfd);
    /* Overwrite any 'a','v','r' triplet in the file with zeros — this
     * is overkill but reliably defeats the heuristic in
     * elf_scan_deviceinfo_note() without needing to locate the note. */
    int xfd = open(tmppath, O_RDWR);
    TEST_ASSERT_NOT_EQUAL(-1, xfd);
    off_t sz = lseek(xfd, 0, SEEK_END);
    TEST_ASSERT_GREATER_THAN(0, (long)sz);
    lseek(xfd, 0, SEEK_SET);
    char *all = malloc((size_t)sz);
    TEST_ASSERT_NOT_NULL(all);
    TEST_ASSERT_EQUAL_INT((ssize_t)sz, read(xfd, all, (size_t)sz));
    for (off_t i = 0; i + 3 < sz; i++) {
        if (all[i] == 'a' && all[i+1] == 'v' && all[i+2] == 'r'
            && all[i+3] >= '0' && all[i+3] <= '9') {
            all[i] = all[i+1] = all[i+2] = 0;
        }
    }
    lseek(xfd, 0, SEEK_SET);
    TEST_ASSERT_EQUAL_INT((ssize_t)sz, write(xfd, all, (size_t)sz));
    free(all);
    close(xfd);

    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    int r = elf_open(tmppath, &ctx);
    unlink(tmppath);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_STRING("", ctx.device_name);
    elf_close(&ctx);
}

/* ── Test runner ─────────────────────────────────────────────────────── */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_elf_open_accepts_valid_avr_elf32_binary);
    RUN_TEST(test_elf_open_returns_minus1_on_invalid_elf_magic);
    RUN_TEST(test_elf_open_returns_minus1_on_wrong_machine_type);
    RUN_TEST(test_elf_open_loads_symtab_and_strtab_into_heap_buffers);
    RUN_TEST(test_elf_open_frees_partial_allocs_and_returns_minus1_on_malloc_failure);
    RUN_TEST(test_elf_find_avros_tables_performs_single_linear_scan);
    RUN_TEST(test_elf_find_avros_tables_populates_all_7_avros_sentinel_fields);
    RUN_TEST(test_elf_flash_addr_applies_vma_minus_base_over_2_formula);
    RUN_TEST(test_elf_flash_addr_all_avros_symbol_addresses_use_word_formula);
    RUN_TEST(test_elf_find_avros_tables_returns_0_on_partial_symbol_match);
    RUN_TEST(test_elf_find_avros_tables_zero_initialises_absent_symbol_fields);
    RUN_TEST(test_elf_close_frees_symtab_strtab_and_closes_fd);
    RUN_TEST(test_elf_close_safe_on_partially_initialised_context);
    RUN_TEST(test_elf_open_sets_flash_base_and_sram_base_from_pt_load_segments);
    RUN_TEST(test_elf_open_extracts_device_name_from_deviceinfo_note);
    RUN_TEST(test_elf_open_leaves_device_name_empty_when_note_absent);
    return UNITY_END();
}
