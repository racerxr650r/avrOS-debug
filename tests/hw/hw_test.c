/* tests/hw/hw_test.c — on-target hardware integration tests
 *
 * Validates avr-updi-gdb behaviours that PTY-based unit tests cannot prove:
 * real UART timing, real UPDI silicon responses, the single-wire combiner
 * electrical path, and end-to-end pipelines.
 *
 * Invocation (manual only — never wired into `make test`):
 *
 *     build/hw_test [--port DEV] [--device-id "HH HH HH"]
 *                   [--flash-page ADDR] [--sram-addr ADDR] [--addr-24bit ADDR]
 *                   [--with-nvm] [--with-rsp]
 *                   [--nvm-elf PATH]
 *                   [--elf PATH] [--rsp-port N]
 *                   [DEV]
 *
 * Defaults come from environment variables HW_PORT, HW_DEVICE_ID,
 * HW_FLASH_PAGE_ADDR, HW_SRAM_ADDR, HW_ADDR_24BIT, HW_TEST_ELF,
 * HW_TEST_NVM_ELF, HW_RSP_PORT.
 * If the first positional argument is supplied it overrides --port / $HW_PORT.
 *
 * Exit code: 0 if every non-skipped case passed, 1 otherwise.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "updi.h"
#include "elf.h"

#define DEF_PORT            "/dev/ttyAMA2"
#define DEF_FLASH_PAGE      0x7E00u   /* last page of 32 KiB AVR128DA28 */
#define DEF_SRAM_ADDR       0x7000u   /* default SRAM scratch addr; mid-RAM
                                       * for AVR128DA (RAM 0x4000-0x7FFF).
                                       * Override with --sram-addr /
                                       * HW_SRAM_ADDR if a different MCU or
                                       * if the running app uses this region. */
#define DEF_RSP_PORT        1234
#define DEF_BAUD            225000
#define DEF_NVM_ELF         "build/fixtures/all_nvm.elf"

/* Phase-8 fixture payload metadata. The fixture (tests/fixtures/all_nvm.c)
 * places 16 bytes in .eeprom and 16 bytes in .user_signatures. Group E
 * extracts them at runtime via the ELF section header table and writes
 * them to the matching UPDI windows on real silicon. */
#define E_EEPROM_OFF        0x00u   /* offset into UPDI EEPROM window  */
#define E_USERROW_OFF       0x00u   /* offset into UPDI USERROW window */
#define E_PAYLOAD_LEN       16u

/* ── Configuration ───────────────────────────────────────────────────── */
typedef struct {
    const char *port;
    int         baud;
    uint8_t     expect_id[3];
    bool        check_id;
    uint32_t    flash_page;     /* byte addr of scratch FLASH page (C group)  */
    uint32_t    sram_addr;      /* byte addr of scratch SRAM region (B group) */
    uint32_t    addr_24bit;     /* 0 = skip B3 */
    bool        with_nvm;
    bool        with_rsp;
    const char *nvm_elf;        /* fixture ELF for Group E NVM payloads      */
    const char *elf_path;       /* required for RSP server child              */
    int         rsp_port;
    int         verbose;        /* 0 = quiet, 1 = per-step, 2 = +full hex     */
} HwCfg;

/* ── Result accounting ─────────────────────────────────── */
static int g_pass, g_fail, g_skip;
static int g_verbose;           /* mirror of cfg.verbose for VLOG() */

#define VLOG(level, ...)  do { if (g_verbose >= (level)) {                  \
        printf("hw-test:   . "); printf(__VA_ARGS__);                       \
    } } while (0)

/* ── Hex dumpers ─────────────────────────────────────────── */
static void hex_row(const char *tag, const uint8_t *buf,
                    size_t off, size_t row_len, size_t total)
{
    printf("hw-test:     %s @%04zX:", tag, off);
    for (size_t i = 0; i < row_len && off + i < total; i++)
        printf(" %02X", buf[off + i]);
    printf("\n");
}

static void dump_diff(const uint8_t *w, const uint8_t *r, size_t len,
                      size_t window)
{
    /* Find first mismatch. */
    size_t i = 0;
    while (i < len && w[i] == r[i]) i++;
    if (i == len) return;

    /* Count mismatches. */
    size_t miscount = 0;
    for (size_t k = 0; k < len; k++) if (w[k] != r[k]) miscount++;
    printf("hw-test:     first mismatch @offset %zu of %zu  "
           "(%zu/%zu bytes differ, %.1f%%)\n",
           i, len, miscount, len, 100.0 * (double)miscount / (double)len);

    /* Print up to `window` bytes either side, aligned to 16. */
    size_t start = (i > window) ? (i - window) & ~(size_t)0xF : 0;
    size_t end   = (i + window < len) ? i + window : len;
    for (size_t off = start; off < end; off += 16) {
        size_t row = (end - off > 16) ? 16 : end - off;
        hex_row("wrote", w, off, row, len);
        hex_row("read ", r, off, row, len);
        /* Mark mismatching columns. */
        printf("hw-test:         diff:");
        for (size_t k = 0; k < row; k++)
            printf(" %s", w[off + k] != r[off + k] ? "^^" : "..");
        printf("\n");
    }
}

/* ── Time helper ─────────────────────────────────────────────────────── */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ── Reporting ───────────────────────────────────────────────────────── */
static void report_pass(const char *id, const char *desc, double ms)
{
    printf("hw-test: %-4s %-46s PASS  (%6.1f ms)\n", id, desc, ms);
    g_pass++;
}
static void report_fail(const char *id, const char *desc, const char *why)
{
    printf("hw-test: %-4s %-46s FAIL  %s\n", id, desc, why);
    g_fail++;
}
static void report_skip(const char *id, const char *desc, const char *why)
{
    printf("hw-test: %-4s %-46s SKIP  %s\n", id, desc, why);
    g_skip++;
}

/* ── Hex-triplet parser: "1E 97 08" / "1E:97:08" / "1E9708" ──────────── */
static bool parse_id3(const char *s, uint8_t out[3])
{
    int n = 0;
    const char *p = s;
    while (*p && n < 3) {
        while (*p == ' ' || *p == ':' || *p == ',') ++p;
        if (!*p) break;
        unsigned v; int got = 0;
        if (sscanf(p, "%2x%n", &v, &got) != 1 || got == 0) return false;
        out[n++] = (uint8_t)v;
        p += got;
    }
    return n == 3;
}

/* ──────────────────────────────────────────────────────────────────── *
 *  Group A — link & identity (non-destructive)                         *
 * ──────────────────────────────────────────────────────────────────── */

static int run_groupA(const HwCfg *cfg, int fd, UpdiDeviceInfo *info)
{
    char why[128];
    double t0;

    /* A1: port openable + cold-start handshake completed (open already done). */
    if (fd < 0) {
        report_fail("A1", "port-open + cold-start handshake",
                    "updi_open() returned -1");
        return -1;
    }
    report_pass("A1", "port-open + cold-start handshake", 0.0);

    /* A2: device-info read succeeds (single round-trip exercise of link).    */
    t0 = now_ms();
    int rc = updi_read_device_info(fd, info);
    if (rc != 0) {
        snprintf(why, sizeof why, "updi_read_device_info() failed at step '%s'",
                 info->fail_op ? info->fail_op : "?");
        report_fail("A2", "device-info read", why);
        return -1;
    }
    report_pass("A2", "device-info read", now_ms() - t0);

    if (g_verbose >= 1) {
        VLOG(1, "deviceid     : %02X %02X %02X\n",
             info->device_id[0], info->device_id[1], info->device_id[2]);
        VLOG(1, "revid        : 0x%02X\n", info->revid);
        printf("hw-test:   . sernum       :");
        for (int i = 0; i < 16; i++) printf(" %02X", info->serial[i]);
        printf("\n");
        VLOG(1, "asi_sys_status: 0x%02X\n", info->asi_sys_status);
        VLOG(1, "asi_key_status: 0x%02X\n", info->asi_key_status);
        VLOG(1, "asi_statusb   : 0x%02X\n", info->asi_statusb);
    }

    /* A3: DEVICEID matches expected (skip if no expectation supplied). */
    if (cfg->check_id) {
        if (memcmp(info->device_id, cfg->expect_id, 3) == 0) {
            char d[40];
            snprintf(d, sizeof d, "deviceid %02X %02X %02X",
                     info->device_id[0], info->device_id[1], info->device_id[2]);
            report_pass("A3", d, 0.0);
        } else {
            snprintf(why, sizeof why,
                     "got %02X %02X %02X, want %02X %02X %02X",
                     info->device_id[0], info->device_id[1], info->device_id[2],
                     cfg->expect_id[0], cfg->expect_id[1], cfg->expect_id[2]);
            report_fail("A3", "deviceid check", why);
        }
    } else {
        report_skip("A3", "deviceid check", "no --device-id supplied");
    }

    /* A4: DEVICEID is non-zero (sanity). */
    if (info->device_id[0] || info->device_id[1] || info->device_id[2])
        report_pass("A4", "deviceid non-zero", 0.0);
    else
        report_fail("A4", "deviceid non-zero", "all bytes 0x00");

    /* A5: REVID is a printable letter or '?'+digit form. */
    if (info->revid > 0)
        report_pass("A5", "revid byte readable", 0.0);
    else
        report_fail("A5", "revid byte readable", "revid==0");

    /* A6: SERNUM is 16 bytes and not all-zero (datasheet §7.6.2.3). */
    int nonzero = 0;
    for (int i = 0; i < 16; i++) if (info->serial[i]) { nonzero = 1; break; }
    if (nonzero)
        report_pass("A6", "sernum non-zero (16 bytes)", 0.0);
    else
        report_fail("A6", "sernum non-zero (16 bytes)", "all 16 bytes 0x00");

    /* A7: ASI_SYS_STATUS: NVMPROG bit (b3) should be clear in normal state.  */
    if ((info->asi_sys_status & 0x08u) == 0)
        report_pass("A7", "asi_sys_status: NVMPROG clear", 0.0);
    else
        report_fail("A7", "asi_sys_status: NVMPROG clear",
                    "target stuck in NVMPROG");
    return 0;
}

/* ──────────────────────────────────────────────────────────────────── *
 *  Group B — memory access                                             *
 * ──────────────────────────────────────────────────────────────────── */

/* SRAM round-trip helper. addr must point inside writable RAM.
 * Return codes:  0 ok, -1 UPDI write err, -2 alloc, -3 mismatch, -4 read err.
 * On -3, when verbose, prints a diff window of write vs read buffers.
 * `wrote_err` / `read_err` outputs are optional. */
static int sram_roundtrip(int fd, uint32_t addr, size_t len, uint8_t seed,
                          const char *tag)
{
    uint8_t *w = malloc(len);
    uint8_t *r = malloc(len);
    if (!w || !r) { free(w); free(r); return -2; }
    for (size_t i = 0; i < len; i++) w[i] = (uint8_t)(seed + i);
    memset(r, 0xCC, len);   /* sentinel so we can see if read populated buf */

    VLOG(1, "%s write %zu B @ 0x%05X (seed 0x%02X)\n", tag, len, addr, seed);
    int rc = updi_mem_write(fd, addr, w, len);
    if (rc != 0) {
        VLOG(1, "%s updi_mem_write returned %d (errno=%d %s)\n",
             tag, rc, errno, strerror(errno));
        free(w); free(r); return -1;
    }

    VLOG(1, "%s read  %zu B @ 0x%05X\n", tag, len, addr);
    rc = updi_mem_read(fd, addr, r, len);
    if (rc != 0) {
        VLOG(1, "%s updi_mem_read returned %d (errno=%d %s)\n",
             tag, rc, errno, strerror(errno));
        free(w); free(r); return -4;
    }

    int mismatch = memcmp(w, r, len) ? 1 : 0;
    if (mismatch && g_verbose >= 1) {
        dump_diff(w, r, len, 16);
    }
    free(w); free(r);
    return mismatch ? -3 : 0;
}

static void run_groupB(const HwCfg *cfg, int fd)
{
    char why[128];
    char lbl_b0[64], lbl_b1[64], lbl_b2[64];
    double t0;

    /* AVR128DA SRAM is 0x4000-0x7FFF (16 KiB). The runtime sram_addr is
     * chosen by --sram-addr / HW_SRAM_ADDR; default is 0x7000 (mid-RAM,
     * usually clear of .data/.bss at the bottom and stack near the top). */
    const uint32_t sram_addr = cfg->sram_addr;
    snprintf(lbl_b0, sizeof lbl_b0, "SRAM 1-byte round-trip @ 0x%05X",
             sram_addr);
    snprintf(lbl_b1, sizeof lbl_b1, "SRAM round-trip 64 B @ 0x%05X",
             sram_addr);
    snprintf(lbl_b2, sizeof lbl_b2, "SRAM round-trip 600 B @ 0x%05X (multi-burst)",
             sram_addr);

    /* B0: single-byte round-trip — isolates burst/repeat issues from
     * basic ST/LD framing. If B0 passes but B1 fails, the issue is in
     * the REPEAT path (or live CPU writes); if B0 fails too, it's a
     * fundamental link/silicon problem. */
    t0 = now_ms();
    {
        uint8_t w = 0xA5, r = 0xCC;
        int rc = updi_mem_write(fd, sram_addr, &w, 1);
        if (rc == 0) rc = updi_mem_read(fd, sram_addr, &r, 1);
        if (rc != 0) {
            snprintf(why, sizeof why, "UPDI error rc=%d errno=%d", rc, errno);
            report_fail("B0", lbl_b0, why);
        } else if (r != w) {
            snprintf(why, sizeof why,
                     "wrote 0x%02X, read 0x%02X (CPU may be writing this byte)",
                     w, r);
            report_fail("B0", lbl_b0, why);
        } else {
            report_pass("B0", lbl_b0, now_ms() - t0);
        }
    }

    /* B1: 64-byte SRAM round-trip (single ST_PTR_WORD burst). */
    t0 = now_ms();
    int rc = sram_roundtrip(fd, sram_addr, 64, 0xA5, "B1");
    if (rc == 0)        report_pass("B1", lbl_b1, now_ms() - t0);
    else if (rc == -3)  report_fail("B1", lbl_b1,
                                    "read-back mismatch (see diff above)");
    else if (rc == -4)  report_fail("B1", lbl_b1, "UPDI read error");
    else                report_fail("B1", lbl_b1, "UPDI write error");

    /* B1a: read-only burst from SIGROW (0x1100, fixed ROM). Isolates the
     * read path from any write-side corruption AND from any possibility of
     * the live CPU mutating bytes. If B1 fails AND B1a passes → corruption
     * is in updi_mem_write. If both fail at the same offset → corruption is
     * in the read/REPEAT/LD path (or UART RX path on the host). */
    t0 = now_ms();
    {
        uint8_t r1[64], r2[64];
        memset(r1, 0xCC, sizeof r1);
        memset(r2, 0xDD, sizeof r2);
        int rc1 = updi_mem_read(fd, 0x1100u, r1, sizeof r1);
        int rc2 = updi_mem_read(fd, 0x1100u, r2, sizeof r2);
        if (rc1 != 0 || rc2 != 0) {
            report_fail("B1a", "SIGROW 64 B read-only x2",
                        "updi_mem_read returned error");
        } else if (memcmp(r1, r2, sizeof r1) != 0) {
            report_fail("B1a", "SIGROW 64 B read-only x2",
                        "two reads of fixed ROM differ (see diff)");
            if (g_verbose >= 1) dump_diff(r1, r2, sizeof r1, 16);
        } else {
            report_pass("B1a", "SIGROW 64 B read-only x2 (identical)",
                        now_ms() - t0);
            if (g_verbose >= 1) {
                hex_row("sigrow", r1,  0, 16, sizeof r1);
                hex_row("sigrow", r1, 16, 16, sizeof r1);
                hex_row("sigrow", r1, 32, 16, sizeof r1);
                hex_row("sigrow", r1, 48, 16, sizeof r1);
            }
        }
    }

    /* B1b: constant-value SRAM round-trip. If corruption at offsets 10-11
     * happens with all bytes = 0xAA, the failure is value-independent
     * (UART/timing); if every byte reads back as 0xAA the failure is in
     * the address/index, not the value. */
    t0 = now_ms();
    {
        uint8_t w[64], r[64];
        memset(w, 0xAA, sizeof w);
        memset(r, 0xCC, sizeof r);
        int rcw = updi_mem_write(fd, sram_addr, w, sizeof w);
        int rcr = (rcw == 0) ? updi_mem_read(fd, sram_addr, r, sizeof r) : -1;
        if (rcw != 0 || rcr != 0) {
            report_fail("B1b", "SRAM constant-0xAA 64 B round-trip",
                        "UPDI error");
        } else if (memcmp(w, r, sizeof r) != 0) {
            int wrong = 0; size_t first = sizeof r;
            for (size_t i = 0; i < sizeof r; i++)
                if (r[i] != 0xAA) { wrong++; if (first == sizeof r) first = i; }
            char buf[96];
            snprintf(buf, sizeof buf,
                     "%d/%zu bytes != 0xAA, first @offset %zu = 0x%02X",
                     wrong, sizeof r, first, r[first]);
            report_fail("B1b", "SRAM constant-0xAA 64 B round-trip", buf);
            if (g_verbose >= 1) dump_diff(w, r, sizeof r, 16);
        } else {
            report_pass("B1b", "SRAM constant-0xAA 64 B round-trip",
                        now_ms() - t0);
        }
    }

    /* B1c: repeatability — write once, read twice. If both reads return
     * the same (wrong) bytes, the corruption is deterministic (UART/timing
     * artifact). If they differ, the running CPU may be mutating SRAM. */
    t0 = now_ms();
    {
        uint8_t w[64], r1[64], r2[64];
        for (size_t i = 0; i < sizeof w; i++) w[i] = (uint8_t)(0xC3 ^ i);
        memset(r1, 0xCC, sizeof r1);
        memset(r2, 0xDD, sizeof r2);
        int rcw  = updi_mem_write(fd, sram_addr, w, sizeof w);
        int rcr1 = (rcw == 0)  ? updi_mem_read(fd, sram_addr, r1, sizeof r1) : -1;
        int rcr2 = (rcr1 == 0) ? updi_mem_read(fd, sram_addr, r2, sizeof r2) : -1;
        if (rcw != 0 || rcr1 != 0 || rcr2 != 0) {
            report_fail("B1c", "SRAM write-once read-twice", "UPDI error");
        } else if (memcmp(r1, r2, sizeof r1) != 0) {
            report_fail("B1c", "SRAM write-once read-twice",
                        "two reads differ → live CPU is writing this region; "
                        "pick another address via --sram-addr / HW_SRAM_ADDR");
            if (g_verbose >= 1) dump_diff(r1, r2, sizeof r1, 16);
        } else if (memcmp(w, r1, sizeof w) != 0) {
            report_fail("B1c", "SRAM write-once read-twice",
                        "deterministic mismatch (UART/REPEAT artifact)");
            if (g_verbose >= 1) dump_diff(w, r1, sizeof w, 16);
        } else {
            report_pass("B1c", "SRAM write-once read-twice", now_ms() - t0);
        }
    }

    /* B2: 600-byte SRAM round-trip (forces multi-burst splitting). */
    t0 = now_ms();
    rc = sram_roundtrip(fd, sram_addr, 600, 0x5C, "B2");
    if (rc == 0)        report_pass("B2", lbl_b2, now_ms() - t0);
    else if (rc == -3)  report_fail("B2", lbl_b2,
                                    "read-back mismatch (see diff above)");
    else if (rc == -4)  report_fail("B2", lbl_b2, "UPDI read error");
    else                report_fail("B2", lbl_b2, "UPDI write error");

    /* B3: 24-bit addressing read above 64 KiB. Skipped unless explicitly
     * enabled (default-skip avoids false negatives on ≤64 KiB parts). */
    if (cfg->addr_24bit == 0) {
        report_skip("B3", "24-bit FLASH read >64 KiB",
                    "set HW_ADDR_24BIT or --addr-24bit to enable");
    } else {
        uint8_t buf[32];
        memset(buf, 0xCC, sizeof buf);
        t0 = now_ms();
        rc = updi_mem_read(fd, cfg->addr_24bit, buf, sizeof buf);
        if (rc == 0) {
            snprintf(why, sizeof why,
                     "ST_PTR_LONG @0x%06X ok (first byte 0x%02X)",
                     cfg->addr_24bit, buf[0]);
            report_pass("B3", why, now_ms() - t0);
            if (g_verbose >= 1) hex_row("data ", buf, 0, 32, 32);
        } else {
            snprintf(why, sizeof why,
                     "updi_mem_read(@0x%06X) returned %d (errno=%d %s)",
                     cfg->addr_24bit, rc, errno, strerror(errno));
            report_fail("B3", "24-bit FLASH read >64 KiB", why);
        }
    }
}

/* ──────────────────────────────────────────────────────────────────── *
 *  Group C — NVM (destructive, --with-nvm)                             *
 * ──────────────────────────────────────────────────────────────────── */

static void run_groupC(const HwCfg *cfg, int fd)
{
    char why[160];
    double t0;
    const uint32_t page = cfg->flash_page;
    const size_t   PAGE_SIZE = 512;

    if (!cfg->with_nvm) {
        report_skip("C1", "FLASH erase + write last page",
                    "opt-in via --with-nvm (DESTRUCTIVE)");
        report_skip("C2", "FLASH read-back verify",
                    "opt-in via --with-nvm (DESTRUCTIVE)");
        return;
    }

    uint8_t *w = malloc(PAGE_SIZE);
    uint8_t *r = malloc(PAGE_SIZE);
    if (!w || !r) {
        report_fail("C1", "FLASH erase + write last page", "malloc failed");
        free(w); free(r); return;
    }
    /* Distinct, recognisable pattern so a stale page can't masquerade. */
    for (size_t i = 0; i < PAGE_SIZE; i++) w[i] = (uint8_t)(0x5A ^ (i & 0xFF));

    /* C1: ERWP page write. */
    t0 = now_ms();
    int rc = updi_nvm_write_flash(fd, page, w, PAGE_SIZE);
    if (rc != 0) {
        snprintf(why, sizeof why,
                 "updi_nvm_write_flash(@0x%05X) returned %d", page, rc);
        report_fail("C1", "FLASH erase + write last page", why);
        report_skip("C2", "FLASH read-back verify",
                    "C1 failed; read-back skipped");
        free(w); free(r); return;
    }
    snprintf(why, sizeof why, "FLASH erase + write @0x%05X (512 B)", page);
    report_pass("C1", why, now_ms() - t0);

    /* C2: read-back via updi_mem_read (mapped-Flash byte access). */
    t0 = now_ms();
    rc = updi_mem_read(fd, page, r, PAGE_SIZE);
    if (rc != 0) {
        snprintf(why, sizeof why,
                 "updi_mem_read(@0x%05X) returned %d", page, rc);
        report_fail("C2", "FLASH read-back verify", why);
    } else if (memcmp(w, r, PAGE_SIZE) != 0) {
        size_t i = 0;
        while (i < PAGE_SIZE && w[i] == r[i]) i++;
        snprintf(why, sizeof why,
                 "mismatch @offset %zu: wrote 0x%02X read 0x%02X",
                 i, w[i], r[i]);
        report_fail("C2", "FLASH read-back verify", why);
    } else {
        report_pass("C2", "FLASH read-back verify", now_ms() - t0);
    }
    free(w); free(r);
}

/* ──────────────────────────────────────────────────────────────────── *
 *  ELF section extractor (used by Group E)                             *
 * ──────────────────────────────────────────────────────────────────── *
 * Returns 0 on success and copies section payload into *buf (capacity
 * *plen on entry; updated to actual size on success). Returns -1 if
 * the section is missing, the ELF is malformed, or the section exceeds
 * the supplied buffer. No allocation. */
static int read_elf_section(const char *path, const char *name,
                            uint8_t *buf, size_t *plen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    Elf32_Ehdr ehdr;
    if (fread(&ehdr, sizeof ehdr, 1, f) != 1)              { fclose(f); return -1; }
    if (memcmp(ehdr.e_ident, "\x7f""ELF", 4) != 0)         { fclose(f); return -1; }
    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0)            { fclose(f); return -1; }
    if (ehdr.e_shstrndx >= ehdr.e_shnum)                   { fclose(f); return -1; }
    if (ehdr.e_shentsize < sizeof(Elf32_Shdr))             { fclose(f); return -1; }

    /* Load the section-name string table. */
    Elf32_Shdr shstr;
    if (fseek(f, (long)(ehdr.e_shoff + (off_t)ehdr.e_shstrndx * ehdr.e_shentsize),
              SEEK_SET) != 0)                              { fclose(f); return -1; }
    if (fread(&shstr, sizeof shstr, 1, f) != 1)            { fclose(f); return -1; }
    if (shstr.sh_size == 0 || shstr.sh_size > 65536u)      { fclose(f); return -1; }

    char *names = malloc(shstr.sh_size);
    if (!names)                                            { fclose(f); return -1; }
    if (fseek(f, (long)shstr.sh_offset, SEEK_SET) != 0 ||
        fread(names, 1, shstr.sh_size, f) != shstr.sh_size) {
        free(names); fclose(f); return -1;
    }

    /* Walk section headers, find the requested name. */
    Elf32_Shdr sh;
    int found = 0;
    for (uint16_t i = 0; i < ehdr.e_shnum; i++) {
        if (fseek(f, (long)(ehdr.e_shoff + (off_t)i * ehdr.e_shentsize),
                  SEEK_SET) != 0) break;
        if (fread(&sh, sizeof sh, 1, f) != 1) break;
        if (sh.sh_name >= shstr.sh_size) continue;
        if (strcmp(names + sh.sh_name, name) != 0) continue;
        if (sh.sh_size > *plen) { free(names); fclose(f); return -1; }
        if (fseek(f, (long)sh.sh_offset, SEEK_SET) != 0) break;
        if (fread(buf, 1, sh.sh_size, f) != sh.sh_size)  break;
        *plen = sh.sh_size;
        found = 1;
        break;
    }
    free(names);
    fclose(f);
    return found ? 0 : -1;
}

/* ──────────────────────────────────────────────────────────────────── *
 *  Group E — Non-FLASH NVM windows (Phase 8, --with-nvm, DESTRUCTIVE)  *
 * ──────────────────────────────────────────────────────────────────── *
 * Verifies the per-window NVM writers added in Phase 8:
 *   E1  EEPROM:   updi_nvm_write_eeprom   + read-back via updi_mem_read
 *   E2  USERROW:  updi_nvm_write_userrow  + read-back via updi_mem_read
 *   E3  LOCKBITS safety interlock — refuses non-unlock pattern without
 *       --allow-lock-updi opt-in (non-destructive: returns before writing).
 *
 * Payload bytes for E1/E2 come from the all_nvm.elf fixture so the
 * same toolchain (avr-gcc) that a real user runs produces the bytes
 * exercised against silicon. */
static void run_groupE(const HwCfg *cfg, int fd)
{
    char why[200];
    double t0;

    if (!cfg->with_nvm) {
        report_skip("E1", "EEPROM write + read-back",
                    "opt-in via --with-nvm (DESTRUCTIVE)");
        report_skip("E2", "USERROW write + read-back",
                    "opt-in via --with-nvm (DESTRUCTIVE)");
        report_skip("E3", "LOCKBITS safety interlock",
                    "opt-in via --with-nvm");
        return;
    }

    const char *elf = cfg->nvm_elf ? cfg->nvm_elf : DEF_NVM_ELF;
    if (access(elf, R_OK) != 0) {
        snprintf(why, sizeof why, "fixture ELF '%s' not readable "
                 "(run `make build/fixtures/all_nvm.elf`)", elf);
        report_fail("E1", "EEPROM write + read-back", why);
        report_skip("E2", "USERROW write + read-back", "E1 prerequisite failed");
        report_skip("E3", "LOCKBITS safety interlock", "E1 prerequisite failed");
        return;
    }

    uint8_t want_eeprom[E_PAYLOAD_LEN], got_eeprom[E_PAYLOAD_LEN];
    uint8_t want_userrow[E_PAYLOAD_LEN], got_userrow[E_PAYLOAD_LEN];
    size_t  n;

    /* E1: EEPROM ------------------------------------------------------ */
    n = sizeof want_eeprom;
    if (read_elf_section(elf, ".eeprom", want_eeprom, &n) != 0 ||
        n != E_PAYLOAD_LEN) {
        snprintf(why, sizeof why,
                 ".eeprom section not found or wrong size in %s", elf);
        report_fail("E1", "EEPROM write + read-back", why);
    } else {
        t0 = now_ms();
        int rc = updi_nvm_write_eeprom(fd, UPDI_EEPROM_BASE + E_EEPROM_OFF,
                                       want_eeprom, E_PAYLOAD_LEN);
        if (rc != 0) {
            snprintf(why, sizeof why,
                     "updi_nvm_write_eeprom returned %d", rc);
            report_fail("E1", "EEPROM write + read-back", why);
        } else {
            rc = updi_mem_read(fd, UPDI_EEPROM_BASE + E_EEPROM_OFF,
                               got_eeprom, E_PAYLOAD_LEN);
            if (rc != 0) {
                snprintf(why, sizeof why,
                         "updi_mem_read(@0x%05X) returned %d",
                         (unsigned)(UPDI_EEPROM_BASE + E_EEPROM_OFF), rc);
                report_fail("E1", "EEPROM write + read-back", why);
            } else if (memcmp(want_eeprom, got_eeprom, E_PAYLOAD_LEN) != 0) {
                dump_diff(want_eeprom, got_eeprom, E_PAYLOAD_LEN, 16);
                report_fail("E1", "EEPROM write + read-back",
                            "read-back mismatch");
            } else {
                report_pass("E1", "EEPROM write + read-back", now_ms() - t0);
            }
        }
    }

    /* E2: USERROW ----------------------------------------------------- */
    n = sizeof want_userrow;
    if (read_elf_section(elf, ".user_signatures", want_userrow, &n) != 0 ||
        n != E_PAYLOAD_LEN) {
        snprintf(why, sizeof why,
                 ".user_signatures section not found or wrong size in %s",
                 elf);
        report_fail("E2", "USERROW write + read-back", why);
    } else {
        t0 = now_ms();
        int rc = updi_nvm_write_userrow(fd, UPDI_USERROW_BASE + E_USERROW_OFF,
                                        want_userrow, E_PAYLOAD_LEN);
        if (rc != 0) {
            snprintf(why, sizeof why,
                     "updi_nvm_write_userrow returned %d", rc);
            report_fail("E2", "USERROW write + read-back", why);
        } else {
            rc = updi_mem_read(fd, UPDI_USERROW_BASE + E_USERROW_OFF,
                               got_userrow, E_PAYLOAD_LEN);
            if (rc != 0) {
                snprintf(why, sizeof why,
                         "updi_mem_read(@0x%05X) returned %d",
                         (unsigned)(UPDI_USERROW_BASE + E_USERROW_OFF), rc);
                report_fail("E2", "USERROW write + read-back", why);
            } else if (memcmp(want_userrow, got_userrow, E_PAYLOAD_LEN) != 0) {
                dump_diff(want_userrow, got_userrow, E_PAYLOAD_LEN, 16);
                report_fail("E2", "USERROW write + read-back",
                            "read-back mismatch");
            } else {
                report_pass("E2", "USERROW write + read-back", now_ms() - t0);
            }
        }
    }

    /* E3: LOCKBITS safety interlock (non-destructive) ----------------- *
     * Pass a non-unlock pattern with allow_updi_disable=false. The writer
     * MUST refuse with a non-zero return code (UPDI_ERR_LOCKED) before
     * issuing any NVMCTRL command, leaving the LOCK window untouched. */
    {
        static const uint8_t danger[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        t0 = now_ms();
        int rc = updi_nvm_write_lockbits(fd, UPDI_LOCK_BASE,
                                         danger, sizeof danger,
                                         false /* allow_updi_disable */);
        if (rc == 0) {
            report_fail("E3", "LOCKBITS safety interlock",
                        "writer returned 0 — UPDI may have been disabled!");
        } else {
            report_pass("E3", "LOCKBITS safety interlock", now_ms() - t0);
        }
    }
}

/* ──────────────────────────────────────────────────────────────────── *
 *  Group D — RSP server smoke (--with-rsp)                             *
 * ──────────────────────────────────────────────────────────────────── */

static int tcp_connect_retry(int port, int retries_ms)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    int elapsed = 0;
    while (elapsed < retries_ms) {
        if (connect(sock, (struct sockaddr *)&sa, sizeof sa) == 0) return sock;
        struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50 ms */
        nanosleep(&ts, NULL);
        elapsed += 50;
    }
    close(sock);
    return -1;
}

/* Single-shot select-then-read with millisecond timeout.  Retained for
 * future cases that need a one-byte probe; D2/D3 use read_rsp_packet()
 * instead so they drain the full server reply.                          */
__attribute__((unused))
static ssize_t read_with_timeout(int fd, void *buf, size_t cap, int timeout_ms)
{
    fd_set r;
    FD_ZERO(&r); FD_SET(fd, &r);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int n = select(fd + 1, &r, NULL, NULL, &tv);
    if (n <= 0) return -1;
    return read(fd, buf, cap);
}

/* Read bytes from fd into buf until a full RSP packet ('$' ... '#' XX XX)
 * has been seen, the buffer fills, or timeout_ms elapses with no further
 * progress.  Returns the number of bytes accumulated (0 on timeout-with-
 * nothing, >0 otherwise).
 *
 * Unlike read_with_timeout(), this drains all bytes that the server has
 * queued — necessary because select() may wake on the lone '+' ack that
 * precedes the packet, and a single read() would consume only the ack.  */
static ssize_t read_rsp_packet(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t got = 0;
    int    have_dollar = 0;
    int    have_hash = 0;
    size_t after_hash = 0;
    double t0 = now_ms();
    while (got + 1 < cap) {
        int remain = timeout_ms - (int)(now_ms() - t0);
        if (remain <= 0) break;
        fd_set r;
        FD_ZERO(&r); FD_SET(fd, &r);
        struct timeval tv = { remain / 1000, (remain % 1000) * 1000 };
        int sel = select(fd + 1, &r, NULL, NULL, &tv);
        if (sel <= 0) break;
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            char c = buf[got + (size_t)i];
            if (!have_dollar) {
                if (c == '$') have_dollar = 1;
            } else if (!have_hash) {
                if (c == '#') have_hash = 1;
            } else {
                ++after_hash;
            }
        }
        got += (size_t)n;
        if (have_dollar && have_hash && after_hash >= 2) break;
    }
    buf[got] = 0;
    return (ssize_t)got;
}

/* Best-effort drain of any pending bytes on the socket (non-blocking). */
static void drain_socket(int fd)
{
    char tmp[256];
    for (;;) {
        fd_set r;
        FD_ZERO(&r); FD_SET(fd, &r);
        struct timeval tv = { 0, 10 * 1000 };  /* 10 ms */
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) return;
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) return;
    }
}

static void run_groupD(const HwCfg *cfg)
{
    char why[160];

    if (!cfg->with_rsp) {
        report_skip("D1", "RSP TCP accept",         "opt-in via --with-rsp");
        report_skip("D2", "RSP qSupported reply",   "opt-in via --with-rsp");
        report_skip("D3", "RSP Ctrl-C interrupt",   "opt-in via --with-rsp");
        return;
    }
    if (!cfg->elf_path || access(cfg->elf_path, R_OK) != 0) {
        report_skip("D1", "RSP TCP accept",
                    "no readable --elf supplied (HW_TEST_ELF)");
        report_skip("D2", "RSP qSupported reply",
                    "no readable --elf supplied (HW_TEST_ELF)");
        report_skip("D3", "RSP Ctrl-C interrupt",
                    "no readable --elf supplied (HW_TEST_ELF)");
        return;
    }

    /* Spawn: build/avr-updi-gdb --port <N> <serial> <elf>                  */
    pid_t child = fork();
    if (child < 0) {
        report_fail("D1", "RSP TCP accept", "fork failed");
        return;
    }
    if (child == 0) {
        char portbuf[16];
        snprintf(portbuf, sizeof portbuf, "%d", cfg->rsp_port);
        /* Redirect server stderr/stdout to /dev/null to keep output clean. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("build/avr-updi-gdb", "avr-updi-gdb",
              "--port", portbuf,
              cfg->port, cfg->elf_path, (char *)NULL);
        _exit(127);
    }

    /* D1: TCP accept within 3 s. */
    double t0 = now_ms();
    int sock = tcp_connect_retry(cfg->rsp_port, 3000);
    if (sock < 0) {
        snprintf(why, sizeof why, "could not connect to 127.0.0.1:%d",
                 cfg->rsp_port);
        report_fail("D1", "RSP TCP accept", why);
        kill(child, SIGTERM); waitpid(child, NULL, 0);
        report_skip("D2", "RSP qSupported reply", "D1 failed");
        report_skip("D3", "RSP Ctrl-C interrupt", "D1 failed");
        return;
    }
    report_pass("D1", "RSP TCP accept", now_ms() - t0);

    /* D2: send +$qSupported#37  (checksum of "qSupported" = 0x37).         */
    static const char req[] = "+$qSupported#37";
    t0 = now_ms();
    if (write(sock, req, sizeof req - 1) != (ssize_t)(sizeof req - 1)) {
        report_fail("D2", "RSP qSupported reply", "write failed");
    } else {
        char buf[512];
        ssize_t n = read_rsp_packet(sock, buf, sizeof buf, 1500);
        if (n <= 0) {
            report_fail("D2", "RSP qSupported reply", "no reply within 1500 ms");
        } else {
            /* Expect at minimum a '$' framing the response.               */
            if (memchr(buf, '$', (size_t)n) != NULL)
                report_pass("D2", "RSP qSupported reply", now_ms() - t0);
            else
                report_fail("D2", "RSP qSupported reply",
                            "no '$' in response");
        }
    }

    /* D3: Ctrl-C async interrupt mid-continue.                            *
     * Resume the CPU with '+$c#63', wait briefly so the OCD halt-poll     *
     * loop in dh_continue is actually running, then inject a raw ETX      *
     * (\x03) byte.  The server should halt the CPU and reply with a stop *
     * packet using SIGINT (T02) rather than SIGTRAP (T05).                */
    drain_socket(sock);   /* discard any bytes still queued from D2  */
    t0 = now_ms();
    static const char go[] = "+$c#63";   /* '+' acks the qSupported reply  */
    if (write(sock, go, sizeof go - 1) != (ssize_t)(sizeof go - 1)) {
        report_fail("D3", "RSP Ctrl-C interrupt", "write 'c' packet failed");
    } else {
        /* Give the CPU ~50 ms of run-time before interrupting so the     *
         * test exercises the running-then-interrupted path (not the      *
         * race where the halt poll fires before our \x03 arrives).       */
        struct timespec ts = { 0, 50L * 1000L * 1000L };
        nanosleep(&ts, NULL);

        const char etx = '\x03';
        if (write(sock, &etx, 1) != 1) {
            report_fail("D3", "RSP Ctrl-C interrupt", "write ETX failed");
        } else {
            char buf[256];
            ssize_t n = read_rsp_packet(sock, buf, sizeof buf, 1500);
            if (n <= 0) {
                report_fail("D3", "RSP Ctrl-C interrupt",
                            "no stop packet within 1500 ms");
            } else {
                /* The server prefixes its reply with '+'; skip ahead to  *
                 * the framing '$' and check the signal code.            */
                const char *p = memchr(buf, '$', (size_t)n);
                if (p && (n - (p - buf)) >= 4 && memcmp(p, "$T02", 4) == 0) {
                    report_pass("D3", "RSP Ctrl-C interrupt", now_ms() - t0);
                } else {
                    snprintf(why, sizeof why,
                             "expected $T02..., got %.*s",
                             (int)(n > 40 ? 40 : (int)n), buf);
                    report_fail("D3", "RSP Ctrl-C interrupt", why);
                }
            }
        }
    }

    close(sock);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
}

/* ──────────────────────────────────────────────────────────────────── *
 *  CLI / env wiring                                                    *
 * ──────────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--port DEV] [--device-id \"HH HH HH\"]\n"
        "          [--flash-page ADDR] [--sram-addr ADDR] [--addr-24bit ADDR]\n"
        "          [--with-nvm] [--with-rsp]\n"
        "          [--nvm-elf PATH]\n"
        "          [--elf PATH] [--rsp-port N]\n"
        "          [-v | --verbose] [-vv]\n"
        "          [DEV]\n"
        "\n"
        "Environment defaults: HW_PORT, HW_DEVICE_ID, HW_FLASH_PAGE_ADDR,\n"
        "                      HW_SRAM_ADDR, HW_ADDR_24BIT, HW_TEST_ELF,\n"
        "                      HW_RSP_PORT, HW_VERBOSE (0/1/2).\n"
        "\n"
        "Note: B1/B2 round-trip tests require an SRAM region that the\n"
        "running application does not actively write to. If B1c reports\n"
        "that two consecutive reads differ, the live CPU is mutating that\n"
        "region; pick another address via --sram-addr / HW_SRAM_ADDR.\n",
        prog);
}

static uint32_t env_u32(const char *key, uint32_t def)
{
    const char *v = getenv(key);
    if (!v || !*v) return def;
    return (uint32_t)strtoul(v, NULL, 0);
}

static int env_int(const char *key, int def)
{
    const char *v = getenv(key);
    if (!v || !*v) return def;
    return (int)strtol(v, NULL, 0);
}

int main(int argc, char *argv[])
{
    HwCfg cfg = {0};
    cfg.port       = getenv("HW_PORT");        if (!cfg.port) cfg.port = DEF_PORT;
    cfg.baud       = DEF_BAUD;
    cfg.flash_page = env_u32("HW_FLASH_PAGE_ADDR", DEF_FLASH_PAGE);
    cfg.sram_addr  = env_u32("HW_SRAM_ADDR",       DEF_SRAM_ADDR);
    cfg.addr_24bit = env_u32("HW_ADDR_24BIT", 0);
    cfg.elf_path   = getenv("HW_TEST_ELF");
    cfg.nvm_elf    = getenv("HW_TEST_NVM_ELF");
    cfg.rsp_port   = env_int("HW_RSP_PORT", DEF_RSP_PORT);
    cfg.verbose    = env_int("HW_VERBOSE", 0);

    const char *eid = getenv("HW_DEVICE_ID");
    if (eid && parse_id3(eid, cfg.expect_id)) cfg.check_id = true;

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--port")        && i+1 < argc) cfg.port = argv[++i];
        else if (!strcmp(a, "--device-id")   && i+1 < argc) {
            if (!parse_id3(argv[++i], cfg.expect_id)) {
                fprintf(stderr, "hw_test: bad --device-id\n"); return 2;
            }
            cfg.check_id = true;
        }
        else if (!strcmp(a, "--flash-page")  && i+1 < argc)
            cfg.flash_page = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--sram-addr")   && i+1 < argc)
            cfg.sram_addr  = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--addr-24bit")  && i+1 < argc)
            cfg.addr_24bit = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--with-nvm"))   cfg.with_nvm = true;
        else if (!strcmp(a, "--with-rsp"))   cfg.with_rsp = true;
        else if (!strcmp(a, "--nvm-elf")     && i+1 < argc) cfg.nvm_elf = argv[++i];
        else if (!strcmp(a, "--elf")         && i+1 < argc) cfg.elf_path = argv[++i];
        else if (!strcmp(a, "--rsp-port")    && i+1 < argc)
            cfg.rsp_port = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose"))  cfg.verbose = 1;
        else if (!strcmp(a, "-vv"))                            cfg.verbose = 2;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]); return 0;
        }
        else if (a[0] != '-' && positional == 0) {
            cfg.port = a;
            positional++;
        } else {
            fprintf(stderr, "hw_test: unrecognised argument '%s'\n", a);
            usage(argv[0]); return 2;
        }
    }

    /* Quick existence check so the failure mode is clean, not a crash. */
    struct stat st;
    if (stat(cfg.port, &st) != 0 || !S_ISCHR(st.st_mode)) {
        fprintf(stderr,
                "hw_test: skip — '%s' is not a character device "
                "(set HW_PORT or pass the port as argument)\n", cfg.port);
        return 0;
    }

    g_verbose = cfg.verbose;
    printf("hw-test: port=%s baud=%d flash_page=0x%05X sram=0x%05X "
           "addr_24bit=0x%X with_nvm=%d with_rsp=%d verbose=%d\n",
           cfg.port, cfg.baud, cfg.flash_page, cfg.sram_addr,
           cfg.addr_24bit, cfg.with_nvm, cfg.with_rsp, cfg.verbose);
    printf("hw-test: ───────────────────────────────────────────────────"
           "────────────\n");

    int fd = updi_open(cfg.port, cfg.baud);
    UpdiDeviceInfo info;
    memset(&info, 0, sizeof info);

    (void)run_groupA(&cfg, fd, &info);
    if (fd >= 0) run_groupB(&cfg, fd);
    if (fd >= 0) run_groupC(&cfg, fd);
    if (fd >= 0) run_groupE(&cfg, fd);
    if (fd >= 0) updi_close(fd);    /* free the UART before spawning RSP server */
    run_groupD(&cfg);

    printf("hw-test: ───────────────────────────────────────────────────"
           "────────────\n");
    printf("hw-test: %d passed, %d failed, %d skipped\n",
           g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
