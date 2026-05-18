/* src/main.c — avr-updi-gdb entry point.
 *
 * Owns CLI parsing, top-level select()-based event loop, signal handling,
 * and guaranteed resource teardown (LLR-MAIN-01 .. LLR-MAIN-07). */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#ifdef __linux__
#include <elf.h>
#else
#include "elf.h"
#endif

#include "elf_parser.h"
#include "updi.h"
#include "fsm_mapper.h"
#include "gdb_rsp.h"

/* ── AppConfig (SDD data_dictionary) ──────────────────────────────────── */
typedef struct {
    const char *serial_device;
    const char *elf_path;
    uint16_t    gdb_port;
    int         baud_rate;
    bool        load_flash;
    bool        erase_chip;    /* --erase: DESTRUCTIVE chip erase before load */
    bool        device_info;   /* --device one-shot diagnostic (LLR-MAIN-08) */
    /* fds owned by main; -1 = closed/unset */
    int         listen_fd;
    int         gdb_fd;
    int         updi_fd;
} AppConfig;

/* Externally visible shutdown flag (LLR-MAIN-06). Set by SIGINT/SIGTERM
 * handler and by the RSP "k" packet handler. */
volatile sig_atomic_t g_quit = 0;

/* In UNIT_TEST builds expose internals to test_main.c and rename main()
 * out of the way so tests can supply their own. */
#ifdef UNIT_TEST
# define MAYBE_STATIC
# define MAIN_NAME app_main
#else
# define MAYBE_STATIC static
# define MAIN_NAME main
#endif

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--port <port>] [--baud <baud>] [--erase] [--load] "
        "<serial-device> <elf-file>\n"
        "       %s --device [--baud <baud>] <serial-device> [elf-file]\n"
        "\n"
        "  --erase   DESTRUCTIVE: chip-erase + unlock before --load.\n"
        "            Required on a locked AVR-Dx target before NVMPROG.\n",
        prog, prog);
}

MAYBE_STATIC void parse_args(int argc, char *argv[], AppConfig *cfg);
MAYBE_STATIC void event_loop(AppConfig *cfg, RspHandlers *h);
MAYBE_STATIC int  load_flash_segments(AppConfig *cfg, ElfContext *ctx);
MAYBE_STATIC int  run_device_mode(AppConfig *cfg);
MAYBE_STATIC void sig_handler(int signo);

/* ── Device family lookup (LLR-MAIN-09) ───────────────────────────────── *
 * Signature → family name table for AVR DA/DB devices. Values are the
 * canonical Microchip SIGROW DEVICEID0..2 triplets published in the
 * AVR-Dx Device Family Pack ATDF files. Unknown signatures render as
 * "unknown device" in the diagnostic report.                              */
typedef struct {
    uint8_t     id[3];
    const char *name;
} DeviceFamilyEntry;

static const DeviceFamilyEntry device_family[] = {
    /* AVR128DA */
    { { 0x1E, 0x97, 0x0A }, "AVR128DA28" },
    { { 0x1E, 0x97, 0x09 }, "AVR128DA32" },
    { { 0x1E, 0x97, 0x08 }, "AVR128DA48" },
    { { 0x1E, 0x97, 0x07 }, "AVR128DA64" },
    /* AVR64DA */
    { { 0x1E, 0x96, 0x15 }, "AVR64DA28"  },
    { { 0x1E, 0x96, 0x14 }, "AVR64DA32"  },
    { { 0x1E, 0x96, 0x13 }, "AVR64DA48"  },
    { { 0x1E, 0x96, 0x12 }, "AVR64DA64"  },
    /* AVR32DA */
    { { 0x1E, 0x95, 0x36 }, "AVR32DA28"  },
    { { 0x1E, 0x95, 0x35 }, "AVR32DA32"  },
    { { 0x1E, 0x95, 0x34 }, "AVR32DA48"  },
    /* AVR128DB */
    { { 0x1E, 0x97, 0x0E }, "AVR128DB28" },
    { { 0x1E, 0x97, 0x0D }, "AVR128DB32" },
    { { 0x1E, 0x97, 0x0C }, "AVR128DB48" },
    { { 0x1E, 0x97, 0x0B }, "AVR128DB64" },
    /* AVR64DB */
    { { 0x1E, 0x96, 0x19 }, "AVR64DB28"  },
    { { 0x1E, 0x96, 0x18 }, "AVR64DB32"  },
    { { 0x1E, 0x96, 0x17 }, "AVR64DB48"  },
    { { 0x1E, 0x96, 0x16 }, "AVR64DB64"  },
    /* AVR32DB */
    { { 0x1E, 0x95, 0x3A }, "AVR32DB28"  },
    { { 0x1E, 0x95, 0x39 }, "AVR32DB32"  },
    { { 0x1E, 0x95, 0x38 }, "AVR32DB48"  },
};

static const char *lookup_device_family(const uint8_t id[3])
{
    for (size_t i = 0; i < sizeof(device_family)/sizeof(device_family[0]); i++) {
        if (device_family[i].id[0] == id[0] &&
            device_family[i].id[1] == id[1] &&
            device_family[i].id[2] == id[2])
            return device_family[i].name;
    }
    return "unknown device";
}

MAYBE_STATIC void parse_args(int argc, char *argv[], AppConfig *cfg)
{
    cfg->serial_device = NULL;
    cfg->elf_path      = NULL;
    cfg->gdb_port      = 1234;
    cfg->baud_rate     = 115200;
    cfg->load_flash    = false;
    cfg->erase_chip    = false;
    cfg->device_info   = false;
    cfg->listen_fd     = -1;
    cfg->gdb_fd        = -1;
    cfg->updi_fd       = -1;

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--port") == 0) {
            if (++i >= argc) { usage(argv[0]); exit(1); }
            cfg->gdb_port = (uint16_t)atoi(argv[i]);
        } else if (strcmp(a, "--baud") == 0) {
            if (++i >= argc) { usage(argv[0]); exit(1); }
            cfg->baud_rate = atoi(argv[i]);
        } else if (strcmp(a, "--load") == 0) {
            cfg->load_flash = true;
        } else if (strcmp(a, "--erase") == 0) {
            cfg->erase_chip = true;
        } else if (strcmp(a, "--device") == 0) {
            cfg->device_info = true;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "%s: unrecognised option '%s'\n", argv[0], a);
            usage(argv[0]);
            exit(1);
        } else if (positional == 0) {
            cfg->serial_device = a;
            positional++;
        } else if (positional == 1) {
            cfg->elf_path = a;
            positional++;
        } else {
            fprintf(stderr, "%s: too many arguments\n", argv[0]);
            usage(argv[0]);
            exit(1);
        }
    }

    /* LLR-MAIN-08: --device and --load are mutually exclusive. */
    if (cfg->device_info && cfg->load_flash) {
        fprintf(stderr,
                "%s: error: --device is mutually exclusive with --load\n",
                argv[0]);
        usage(argv[0]);
        exit(1);
    }

    if (cfg->serial_device == NULL) {
        usage(argv[0]);
        exit(1);
    }
    /* --device makes <elf-file> optional (LLR-MAIN-08). */
    if (!cfg->device_info && cfg->elf_path == NULL) {
        usage(argv[0]);
        exit(1);
    }
}

MAYBE_STATIC void sig_handler(int signo)
{
    (void)signo;
    g_quit = 1;
}

MAYBE_STATIC void event_loop(AppConfig *cfg, RspHandlers *h)
{
    char pkt[RSP_PACKET_MAX];

    while (!g_quit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (cfg->listen_fd >= 0) {
            FD_SET(cfg->listen_fd, &rfds);
            maxfd = cfg->listen_fd;
        }
        if (cfg->gdb_fd >= 0) {
            FD_SET(cfg->gdb_fd, &rfds);
            if (cfg->gdb_fd > maxfd) maxfd = cfg->gdb_fd;
            if (cfg->updi_fd >= 0) {
                FD_SET(cfg->updi_fd, &rfds);
                if (cfg->updi_fd > maxfd) maxfd = cfg->updi_fd;
            }
        }

        int n = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (g_quit) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* (1) accept new GDB client when none is connected */
        if (cfg->listen_fd >= 0 && FD_ISSET(cfg->listen_fd, &rfds) &&
            cfg->gdb_fd < 0) {
            int fd = rsp_accept(cfg->listen_fd);
            if (fd >= 0) cfg->gdb_fd = fd;
        }

        /* (2) forward UPDI console traffic to stdout */
        if (cfg->gdb_fd >= 0 && cfg->updi_fd >= 0 &&
            FD_ISSET(cfg->updi_fd, &rfds)) {
            char ubuf[256];
            int got = updi_console_poll(cfg->updi_fd, ubuf, sizeof ubuf);
            if (got > 0) {
                (void)fwrite(ubuf, 1, (size_t)got, stdout);
                fflush(stdout);
            }
        }

        /* (3) RSP packet from GDB → dispatch */
        if (cfg->gdb_fd >= 0 && FD_ISSET(cfg->gdb_fd, &rfds)) {
            int rc = rsp_recv_packet(cfg->gdb_fd, pkt, sizeof pkt);
            if (rc <= 0) {
                rsp_close(cfg->gdb_fd);
                cfg->gdb_fd = -1;
            } else {
                (void)rsp_dispatch(cfg->gdb_fd, pkt, h);
            }
        }
    }
}

/* Iterate PT_LOAD segments and flash each non-SRAM segment.
 * Returns 0 on success, -1 on any read or flash error. */
/* run_device_mode (LLR-MAIN-09): one-shot diagnostic that opens UPDI,
 * reads SIGROW + ASI status via updi_read_device_info(), prints a
 * human-readable report to stdout, and returns the process exit code.
 * Does NOT call rsp_listen(), elf_*(), fsm_*(), or event_loop().      */
MAYBE_STATIC int run_device_mode(AppConfig *cfg)
{
    UpdiDeviceInfo info;
    int            fd;
    int            rc;

    memset(&info, 0, sizeof info);

    fd = updi_open(cfg->serial_device, cfg->baud_rate);
    if (fd < 0) {
        fprintf(stderr,
                "error: updi-open failed for '%s' \u2014 "
                "check wiring, target power, UPDIDIS fuse\n",
                cfg->serial_device);
        return 1;
    }
    cfg->updi_fd = fd;

    rc = updi_read_device_info(fd, &info);
    if (rc < 0) {
        fprintf(stderr,
                "error: %s failed (rc=%d) \u2014 "
                "check wiring, target power, UPDIDIS fuse\n",
                info.fail_op ? info.fail_op : "updi",
                info.fail_errno);
        updi_close(fd);
        cfg->updi_fd = -1;
        return 1;
    }

    printf("Serial device:   %s\n", cfg->serial_device);
    printf("Baud rate:       %d\n", cfg->baud_rate);
    printf("Signature:       %02X %02X %02X\n",
           info.device_id[0], info.device_id[1], info.device_id[2]);
    printf("Family:          %s\n", lookup_device_family(info.device_id));
    {
        /* Datasheet §8.3.2.1: REVID MAJOR encoding 0x01=A, 0x02=B, ...   *
         * MINOR encoding 0x00=0, 0x01=1, ...                              */
        unsigned major = (unsigned)(info.revid >> 4) & 0x0Fu;
        unsigned minor = (unsigned)(info.revid     ) & 0x0Fu;
        if (major == 0u)
            printf("Revision:        ?%u (raw=0x%02X)\n",
                   minor, info.revid);
        else
            printf("Revision:        %c%u\n",
                   (char)('A' + (int)major - 1), minor);
    }
    printf("Serial:          "
           "%02X %02X %02X %02X %02X %02X %02X %02X "
           "%02X %02X %02X %02X %02X %02X %02X %02X\n",
           info.serial[0],  info.serial[1],  info.serial[2],  info.serial[3],
           info.serial[4],  info.serial[5],  info.serial[6],  info.serial[7],
           info.serial[8],  info.serial[9],  info.serial[10], info.serial[11],
           info.serial[12], info.serial[13], info.serial[14], info.serial[15]);
    printf("UPDI status:     SYS_STATUS=0x%02X  KEY_STATUS=0x%02X  STATUSB=0x%02X\n",
           info.asi_sys_status, info.asi_key_status, info.asi_statusb);
    fflush(stdout);

    updi_close(fd);
    cfg->updi_fd = -1;
    return 0;
}

MAYBE_STATIC int load_flash_segments(AppConfig *cfg, ElfContext *ctx)
{
    Elf32_Ehdr ehdr = ctx->ehdr;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        off_t off = (off_t)ehdr.e_phoff +
                    (off_t)i * (off_t)sizeof(Elf32_Phdr);
        if (lseek(ctx->fd, off, SEEK_SET) < 0) return -1;
        if (read(ctx->fd, &ph, sizeof ph) != (ssize_t)sizeof ph) return -1;
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        /* Skip SRAM-only segments (VMA inside SRAM region). */
        if (ctx->sram_base != 0 && ph.p_vaddr >= ctx->sram_base) continue;

        /* updi_nvm_write_flash() requires page-aligned address and length;
         * pad the tail of the segment with 0xFF (erased-flash value) so a
         * non-aligned p_filesz can still be programmed page-by-page. */
        if ((ph.p_vaddr % UPDI_FLASH_PAGE_SIZE) != 0u) {
            fprintf(stderr,
                    "error: segment vaddr 0x%06x not page-aligned\n",
                    (unsigned)ph.p_vaddr);
            return -1;
        }
        uint32_t padded = (ph.p_filesz + UPDI_FLASH_PAGE_SIZE - 1u) &
                          ~(UPDI_FLASH_PAGE_SIZE - 1u);

        uint8_t *buf = malloc(padded);
        if (!buf) return -1;
        memset(buf, 0xFF, padded);
        if (lseek(ctx->fd, (off_t)ph.p_offset, SEEK_SET) < 0 ||
            read(ctx->fd, buf, ph.p_filesz) != (ssize_t)ph.p_filesz) {
            free(buf);
            return -1;
        }
        /* AVR-Dx UPDI memory map: FLASH lives at 0x800000 + flash_offset
         * in the 24-bit unified address space.  avr-gcc links .text at
         * p_vaddr = 0x000000 (program-memory view), so OR in the UPDI
         * FLASH base before driving the NVM controller. */
        uint32_t updi_addr = ph.p_vaddr | UPDI_FLASH_BASE;
        int rc = updi_nvm_write_flash(cfg->updi_fd, updi_addr, buf, padded);
        free(buf);
        if (rc < 0) return -1;
        printf("loaded %u bytes @ 0x%06x → UPDI 0x%06x (padded to %u)\n",
               (unsigned)ph.p_filesz, (unsigned)ph.p_vaddr,
               (unsigned)updi_addr, (unsigned)padded);
    }
    return 0;
}

int MAIN_NAME(int argc, char *argv[])
{
    AppConfig cfg;
    ElfContext elf_ctx;
    AvrOsSymbolIndex idx;
    FsmContext fsm_ctx;
    memset(&elf_ctx, 0, sizeof elf_ctx);
    memset(&idx,     0, sizeof idx);
    memset(&fsm_ctx, 0, sizeof fsm_ctx);
    elf_ctx.fd = -1;

    parse_args(argc, argv, &cfg);

    /* LLR-MAIN-09: --device short-circuits the normal startup path. */
    if (cfg.device_info) {
        return run_device_mode(&cfg);
    }

    /* Signal handlers (LLR-MAIN-06). */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int exit_code = 0;
    bool elf_opened = false;

    if (elf_open(cfg.elf_path, &elf_ctx) < 0) {
        fprintf(stderr, "error: cannot open ELF '%s'\n", cfg.elf_path);
        exit_code = 1;
        goto teardown;
    }
    elf_opened = true;

    /* LLR-MAIN-03: open UPDI first; verify before listening. */
    cfg.updi_fd = updi_open(cfg.serial_device, cfg.baud_rate);
    if (cfg.updi_fd < 0) {
        fprintf(stderr, "error: cannot open UPDI device '%s'\n",
                cfg.serial_device);
        exit_code = 1;
        goto teardown;
    }

    /* LLR-MAIN-04: optional flash load before listener. */
    if (cfg.erase_chip) {
        printf("chip-erase (DESTRUCTIVE) requested\n");
        if (updi_chip_erase(cfg.updi_fd) < 0) {
            fprintf(stderr, "error: chip erase failed\n");
            exit_code = 1;
            goto teardown;
        }
        printf("chip-erase complete; device unlocked\n");
    }
    if (cfg.load_flash) {
        if (load_flash_segments(&cfg, &elf_ctx) < 0) {
            fprintf(stderr, "error: flash load failed\n");
            exit_code = 1;
            goto teardown;
        }
    }

    cfg.listen_fd = rsp_listen(cfg.gdb_port);
    if (cfg.listen_fd < 0) {
        fprintf(stderr, "error: cannot bind GDB listener on port %u\n",
                (unsigned)cfg.gdb_port);
        exit_code = 1;
        goto teardown;
    }

    /* Best-effort session init; failures are tolerated. */
    (void)elf_find_avros_tables(&elf_ctx, &idx);
    (void)fsm_build_thread_list(&fsm_ctx, &idx, cfg.updi_fd);

    int g_thread = -1, c_thread = -1;
    RspContext rctx = {
        .updi_fd    = cfg.updi_fd,
        .gdb_fd_p   = &cfg.gdb_fd,
        .fsm        = &fsm_ctx,
        .idx        = &idx,
        .g_thread_p = &g_thread,
        .c_thread_p = &c_thread,
        .quit_p     = &g_quit,
    };
    RspHandlers handlers;
    rsp_default_handlers(&handlers, &rctx);

    event_loop(&cfg, &handlers);

teardown:
    /* LLR-MAIN-07: gdb_fd → listen_fd → elf_close → updi_close. */
    if (cfg.gdb_fd >= 0)    rsp_close(cfg.gdb_fd);
    if (cfg.listen_fd >= 0) rsp_close(cfg.listen_fd);
    if (elf_opened)         elf_close(&elf_ctx);
    if (cfg.updi_fd >= 0)   updi_close(cfg.updi_fd);
    return exit_code;
}
