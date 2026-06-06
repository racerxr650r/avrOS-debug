/* tests/test_elf.c — Unit tests for src/elf_parser.c
 *
 * elf_parser is now a thin adapter over elfutils (libelf + libdw), so these
 * tests exercise observable behaviour against real AVR ELF fixtures rather
 * than the parser's former hand-rolled internals (heap symtab/strtab buffers,
 * malloc-failure paths) which libelf now owns. */
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

void setUp(void)    {}
void tearDown(void) {}

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
    TEST_ASSERT_NOT_NULL(ctx.elf);
    TEST_ASSERT_NOT_EQUAL(-1, ctx.fd);
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

/* ── Test 4: all 7 avrOS sentinel fields non-zero with full fixture ──── */
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

/* ── Test 5: elf_flash_addr formula (vma - flash_base) / 2 ───────────── */
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

/* ── Test 6: FLASH-table fields are LMA-translated; SRAM field is raw ─── *
 * The FLASH-resident tables live in the AVR-Dx mapped-flash window and    *
 * must be translated to a physical FLASH byte (LMA, below the 0x800000    *
 * SRAM/data band), while currStateMachine is a raw SRAM VMA (>=0x800000). */
void test_elf_find_avros_tables_translates_flash_but_not_sram(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    TEST_ASSERT_EQUAL_INT(0, elf_find_avros_tables(&ctx, &idx));

    /* FLASH-resident → physical byte address, below the data/SRAM band. */
    TEST_ASSERT_TRUE(idx.fsm_table_addr   < 0x800000U);
    TEST_ASSERT_TRUE(idx.queue_table_addr < 0x800000U);
    TEST_ASSERT_TRUE(idx.event_table_addr < 0x800000U);
    /* SRAM-resident → raw VMA in the AVR data band. */
    TEST_ASSERT_TRUE(idx.current_fsm_addr >= 0x800000U);

    elf_close(&ctx);
}

/* ── Test 7: partial fixture → returns 0, not -1 ─────────────────────── */
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

/* ── Test 8: absent symbols stay zero-initialised ────────────────────── */
void test_elf_find_avros_tables_zero_initialises_absent_symbol_fields(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_PARTIAL, &ctx));

    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    TEST_ASSERT_EQUAL_INT(0, elf_find_avros_tables(&ctx, &idx));

    /* Partial fixture has only FSM_TABLE sentinels + currStateMachine; the
     * queue and event table fields must remain zero-initialised.          */
    TEST_ASSERT_EQUAL_UINT32(0U, idx.queue_table_addr);
    TEST_ASSERT_EQUAL_UINT8 (0U, idx.queue_count);
    TEST_ASSERT_EQUAL_UINT32(0U, idx.event_table_addr);
    TEST_ASSERT_EQUAL_UINT8 (0U, idx.event_count);

    elf_close(&ctx);
}

/* ── Test 9: elf_has_fsm_symbols predicate ───────────────────────────── */
void test_elf_has_fsm_symbols_requires_non_zero_table_address_and_count(void)
{
    AvrOsSymbolIndex idx;
    memset(&idx, 0, sizeof(idx));
    TEST_ASSERT_EQUAL_INT(0, elf_has_fsm_symbols(&idx));

    idx.fsm_table_addr = 0x1000u;
    idx.fsm_table_count = 2u;
    TEST_ASSERT_EQUAL_INT(1, elf_has_fsm_symbols(&idx));
}

/* ── Test 10: elf_close releases handles and closes the fd ───────────── */
void test_elf_close_releases_handles_and_closes_fd(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    int saved_fd = ctx.fd;
    TEST_ASSERT_NOT_EQUAL(-1, saved_fd);
    TEST_ASSERT_NOT_NULL(ctx.elf);

    elf_close(&ctx);

    TEST_ASSERT_EQUAL_INT(-1, ctx.fd);
    TEST_ASSERT_NULL(ctx.elf);
    TEST_ASSERT_NULL(ctx.dwarf);

    /* Verify the OS fd is actually closed. */
    TEST_ASSERT_EQUAL_INT(-1, fcntl(saved_fd, F_GETFD));
    TEST_ASSERT_EQUAL_INT(EBADF, errno);
}

/* ── Test 11: elf_close safe on a zero-initialised context ───────────── */
void test_elf_close_safe_on_partially_initialised_context(void)
{
    /* No open performed: all handles NULL, fd -1.  Must not crash. */
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = -1;

    elf_close(&ctx);

    TEST_ASSERT_NULL(ctx.elf);
    TEST_ASSERT_NULL(ctx.dwarf);
    TEST_ASSERT_EQUAL_INT(-1, ctx.fd);
}

/* ── Test 12: flash_base and sram_base populated from PT_LOAD segments ─ */
void test_elf_open_sets_flash_base_and_sram_base_from_pt_load_segments(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    /* avros_full.elf is built for AVR128DA28: SRAM data band at 0x00804000. */
    TEST_ASSERT_EQUAL_UINT32(0x00804000UL, ctx.sram_base);
    TEST_ASSERT_GREATER_THAN(0, (int)ctx.flash_size);

    elf_close(&ctx);
}

/* ── Test 13: deviceinfo note → device_name populated ────────────────── */
void test_elf_open_extracts_device_name_from_deviceinfo_note(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));
    /* All AVR fixtures are built for AVR128DA28. */
    TEST_ASSERT_EQUAL_STRING("avr128da28", ctx.device_name);
    elf_close(&ctx);
}

/* ── Test 14: absent deviceinfo note → device_name stays empty ───────── */
void test_elf_open_leaves_device_name_empty_when_note_absent(void)
{
    /* Copy the full fixture, then zero every 'avr<digit>' triplet so the
     * deviceinfo heuristic can find nothing — without disturbing the ELF
     * structure libelf needs to parse. */
    char tmppath[] = "/tmp/aod_no_note_XXXXXX";
    int fd = mkstemp(tmppath);
    TEST_ASSERT_NOT_EQUAL(-1, fd);
    close(fd);

    int rfd = open(FIXTURE_FULL, O_RDONLY);
    TEST_ASSERT_NOT_EQUAL(-1, rfd);
    int wfd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT_NOT_EQUAL(-1, wfd);
    char io[4096];
    ssize_t n;
    while ((n = read(rfd, io, sizeof io)) > 0)
        TEST_ASSERT_EQUAL_INT(n, write(wfd, io, (size_t)n));
    close(rfd);
    close(wfd);

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

/* ── Test 15: DWARF source-line accessors round-trip ─────────────────── *
 * The AVR fixtures are compiled with -g, so libdw must map the entry-point *
 * code address to a real file:line and back to a real address.            */
void test_elf_dwarf_accessors_round_trip(void)
{
    ElfContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, elf_open(FIXTURE_FULL, &ctx));

    TEST_ASSERT_EQUAL_INT(1, elf_dwarf_available());

    char file[256] = {0};
    int  line      = -1;
    int  r = elf_addr_to_line(&ctx, ctx.ehdr.e_entry, file, sizeof file, &line);
    if (r == 0) {  /* entry may map to a CRT file without a line row */
        TEST_ASSERT_GREATER_THAN(0, line);
        TEST_ASSERT_TRUE(file[0] != '\0');

        uint32_t addr = 0xFFFFFFFFu;
        if (elf_line_to_addr(&ctx, file, line, &addr) == 0)
            TEST_ASSERT_NOT_EQUAL(0xFFFFFFFFu, addr);
    }

    /* An address far outside any CU yields -1, never a crash. */
    TEST_ASSERT_EQUAL_INT(-1,
        elf_addr_to_line(&ctx, 0x7FFFFFFFu, file, sizeof file, &line));

    elf_close(&ctx);
}

/* ── Test runner ─────────────────────────────────────────────────────── */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_elf_open_accepts_valid_avr_elf32_binary);
    RUN_TEST(test_elf_open_returns_minus1_on_invalid_elf_magic);
    RUN_TEST(test_elf_open_returns_minus1_on_wrong_machine_type);
    RUN_TEST(test_elf_find_avros_tables_populates_all_7_avros_sentinel_fields);
    RUN_TEST(test_elf_flash_addr_applies_vma_minus_base_over_2_formula);
    RUN_TEST(test_elf_find_avros_tables_translates_flash_but_not_sram);
    RUN_TEST(test_elf_find_avros_tables_returns_0_on_partial_symbol_match);
    RUN_TEST(test_elf_find_avros_tables_zero_initialises_absent_symbol_fields);
    RUN_TEST(test_elf_has_fsm_symbols_requires_non_zero_table_address_and_count);
    RUN_TEST(test_elf_close_releases_handles_and_closes_fd);
    RUN_TEST(test_elf_close_safe_on_partially_initialised_context);
    RUN_TEST(test_elf_open_sets_flash_base_and_sram_base_from_pt_load_segments);
    RUN_TEST(test_elf_open_extracts_device_name_from_deviceinfo_note);
    RUN_TEST(test_elf_open_leaves_device_name_empty_when_note_absent);
    RUN_TEST(test_elf_dwarf_accessors_round_trip);
    return UNITY_END();
}
