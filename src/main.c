/* src/main.c — avrOSdb entry point.
 *
 * Owns CLI parsing, top-level select()-based event loop, signal handling,
 * and guaranteed resource teardown (LLR-MAIN-01 .. LLR-MAIN-07). */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include <elf.h>

#include "elf_parser.h"
#include "updi.h"
#include "fsm_mapper.h"
#include "dap.h"
#include "gdb_rsp.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ── AppConfig (SDD data_dictionary) ──────────────────────────────────── */
typedef struct {
    const char *serial_device;
    const char *elf_path;
    uint16_t    gdb_port;
    int         baud_rate;
    bool        load_flash;
    bool        erase_chip;    /* --erase: DESTRUCTIVE chip erase before load */
    bool        device_info;   /* --device one-shot diagnostic (LLR-MAIN-08) */
    bool        allow_lock_updi; /* --allow-lock-updi: permit lockbit value
                                  *  that disables UPDI (HLR-047)         */
    bool        prog_mode;     /* --prog: program + verify + exit
                                *  (LLR-MAIN-15, Phase 9)               */
    bool        no_verify;     /* --no-verify: skip read-back verify
                                *  after --load / --prog (LLR-MAIN-14)  */
    bool        no_autobaud;   /* --no-autobaud: skip --device autobaud */
    bool        log_rsp;       /* --log-rsp: detailed logging to stderr */
    bool        allow_erase;   /* --allow-erase: HLR-055 gate for the
                                *  monitor erase / chip-erase verbs   */
    const char *force_device;    /* --force-device=<family>: skip SIGROW
                                  *  autodetect and use the named family  */
    bool        no_introspect;  /* --no-introspect: disable avrOS FSM introspection */
    bool        reset_cpu;       /* --reset: pulse UPDI system reset and exit    */
    bool        dap_mode;        /* --dap: serve DAP instead of RSP (default).
                                  *  --rsp selects RSP explicitly; the two are
                                  *  mutually exclusive (Phase 16).            */
    bool        debug_in_sleep;  /* keep the system clock alive (CLK_REQ) so the
                                  *  OCD survives the target's SLEEP. On by
                                  *  default; --sleep disables it (Phase 20).  */
    /* fds owned by main; -1 = closed/unset */
    int         listen_fd;
    int         gdb_fd;
    int         updi_fd;
} AppConfig;

/* Externally visible shutdown flag (LLR-MAIN-06). Set by SIGINT/SIGTERM
 * handler and by the RSP "k" packet handler. */
volatile sig_atomic_t g_quit = 0;

/* HLR-065 / LLR-MAIN-22: signal-number recorded by sig_handler so the
 * shutdown lifecycle line can name SIGINT vs SIGTERM.  Value 0 means
 * the quit flag was set by some non-signal path (e.g. the RSP `k` /
 * `vKill` packet, or a fatal error in event_loop()).                  */
static volatile sig_atomic_t g_shutdown_signal = 0;

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
        "usage: %s [--rsp | --dap] [--port <port>] [--baud <baud>] [--erase] [--load] "
        "[--no-verify] [--allow-lock-updi] [--force-device=<family>] [--log-rsp] "
        "<serial-device> <elf-file>\n"
        "       %s --prog [--baud <baud>] [--erase] [--no-verify] "
        "[--allow-lock-updi] [--force-device=<family>] [--log-rsp] "
        "<serial-device> <elf-file>\n"
        "       %s --device [--baud <baud>] [--no-autobaud] "
        "[--force-device=<family>] <serial-device> [elf-file]\n"
        "       %s --reset [--baud <baud>] <serial-device>\n"
        "\n"
        "  --erase            DESTRUCTIVE: chip-erase + unlock before --load.\n"
        "                     Required on a locked AVR-Dx target before NVMPROG.\n"
        "  --load             Program every PT_LOAD segment to the matching NVM\n"
        "                     window (FLASH, EEPROM, USERROW, FUSES, LOCK).\n"
        "  --prog             Program + verify the ELF, then exit cleanly\n"
        "                     without entering debug or opening a GDB listener.\n"
        "                     Renders an ANSI progress bar on TTY stdout;\n"
        "                     degrades to per-phase lines when piped.\n"
        "                     Exit code 2 signals verify failure (CI use).\n"
        "  --no-verify        Skip the read-back verify after --load / --prog.\n"
        "  --no-autobaud      Skip the candidate-baud probe in --device mode.\n"
        "  --allow-lock-updi  Permit a lockbit write that disables UPDI.\n"
        "                     Without this flag, only the UPDI-unlock pattern\n"
        "                     0x5CC5C55C is accepted in the LOCK window.\n"
        "  --allow-erase      Permit the GDB-side `monitor erase` /\n"
        "                     `monitor chip-erase` verbs to issue a chip\n"
        "                     erase at runtime.  Off by default.\n"
        "  --force-device=F   Skip SIGROW autodetect and use family F\n"
        "                     (AVR-DA|AVR-DB|AVR-DD|AVR-DU|AVR-SD).\n"
        "                     Only AVR-DA is hardware-validated; the other\n"
        "                     four families are declared from datasheet\n"
        "                     evidence only.\n"
        "  --device           Print target device SIGROW info and exit.\n"
        "  --reset            Pulse the UPDI system reset and exit.\n"
        "                     Requires only <serial-device>; no ELF needed.\n"
        "                     Redundant when combined with --prog.\n"
        "  --log-rsp          Log all incoming and outgoing GDB RSP packets\n"
        "                     to standard error.\n"
        "  --sleep            Allow the target to sleep natively. By default the\n"
        "                     debugger asserts CLK_REQ on entering OCD so the\n"
        "                     system clock survives SLEEP and breakpoints still\n"
        "                     fire after the firmware sleeps; pass --sleep to\n"
        "                     restore native sleep/power behaviour.\n"
        "  --no-introspect    Disable avrOS FSM introspection (the `monitor\n"
        "                     avros` task/state view). The GDB thread model is\n"
        "                     unaffected: the live CPU is always the sole GDB\n"
        "                     thread.\n"
        "  --rsp              Serve the GDB Remote Serial Protocol front-end\n"
        "                     (the default). Mutually exclusive with --dap.\n"
        "  --dap              Serve the Debug Adapter Protocol front-end for\n"
        "                     DAP-native editors (VS Code). Mutually exclusive\n"
        "                     with --rsp.\n",
        prog, prog, prog, prog);
}

MAYBE_STATIC void parse_args(int argc, char *argv[], AppConfig *cfg);
MAYBE_STATIC void event_loop(AppConfig *cfg, RspHandlers *h);
MAYBE_STATIC int  load_segments(AppConfig *cfg, ElfContext *ctx);
MAYBE_STATIC int  verify_segments(AppConfig *cfg, ElfContext *ctx);
MAYBE_STATIC int  run_device_mode(AppConfig *cfg);
MAYBE_STATIC int  run_prog_mode(AppConfig *cfg);
MAYBE_STATIC int  run_reset_mode(AppConfig *cfg);
MAYBE_STATIC void run_autobaud_probe(AppConfig *cfg);
MAYBE_STATIC void sig_handler(int signo);
MAYBE_STATIC void progress_render(const char *phase, const char *window,
                                  unsigned page, unsigned total);
MAYBE_STATIC void progress_finish(void);

/* ── Device family lookup (LLR-MAIN-09) ───────────────────────────────── *
 * Signature → family name table for AVR DA/DB devices. Values are the
 * canonical Microchip SIGROW DEVICEID0..2 triplets published in the
 * AVR-Dx Device Family Pack ATDF files. Unknown signatures render as
 * "unknown device" in the diagnostic report.                              */
typedef struct {
    uint8_t     id[3];
    const char *name;
    DeviceMemoryLayout map;
} DeviceFamilyEntry;

static const DeviceFamilyEntry device_family[] = {
    /* AVR128DA */
    { { 0x1E, 0x97, 0x0A }, "AVR128DA28", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x97, 0x09 }, "AVR128DA32", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x97, 0x08 }, "AVR128DA48", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x97, 0x07 }, "AVR128DA64", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    /* AVR64DA */
    { { 0x1E, 0x96, 0x15 }, "AVR64DA28", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x96, 0x14 }, "AVR64DA32", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x96, 0x13 }, "AVR64DA48", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x96, 0x12 }, "AVR64DA64", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    /* AVR32DA */
    { { 0x1E, 0x95, 0x36 }, "AVR32DA28", {
      .flash_base = 0x000000, .flash_size = 32768,
      .sram_base = 0x804000, .sram_size = 4096,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x95, 0x35 }, "AVR32DA32", {
      .flash_base = 0x000000, .flash_size = 32768,
      .sram_base = 0x804000, .sram_size = 4096,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x95, 0x34 }, "AVR32DA48", {
      .flash_base = 0x000000, .flash_size = 32768,
      .sram_base = 0x804000, .sram_size = 4096,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    /* AVR128DB */
    { { 0x1E, 0x97, 0x0E }, "AVR128DB28", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x97, 0x0D }, "AVR128DB32", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x97, 0x0C }, "AVR128DB48", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x97, 0x0B }, "AVR128DB64", {
      .flash_base = 0x000000, .flash_size = 131072,
      .sram_base = 0x804000, .sram_size = 16384,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    /* AVR64DB */
    { { 0x1E, 0x96, 0x19 }, "AVR64DB28", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x96, 0x18 }, "AVR64DB32", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x96, 0x17 }, "AVR64DB48", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x96, 0x16 }, "AVR64DB64", {
      .flash_base = 0x000000, .flash_size = 65536,
      .sram_base = 0x804000, .sram_size = 8192,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    /* AVR32DB */
    { { 0x1E, 0x95, 0x3A }, "AVR32DB28", {
      .flash_base = 0x000000, .flash_size = 32768,
      .sram_base = 0x804000, .sram_size = 4096,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x95, 0x39 }, "AVR32DB32", {
      .flash_base = 0x000000, .flash_size = 32768,
      .sram_base = 0x804000, .sram_size = 4096,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
    { { 0x1E, 0x95, 0x38 }, "AVR32DB48", {
      .flash_base = 0x000000, .flash_size = 32768,
      .sram_base = 0x804000, .sram_size = 4096,
      .eeprom_base = 0x810000, .eeprom_size = 512,
      .userrow_base = 0x850000, .userrow_size = 32,
      .fuses_base = 0x820000, .fuses_size = 16,
      .lock_base = 0x830000, .lock_size = 4,
      .sigrow_base = 0x840000, .sigrow_size = 64
    } },
};

static const DeviceFamilyEntry *lookup_device_by_id(const uint8_t id[3])
{
    for (size_t i = 0; i < sizeof(device_family)/sizeof(device_family[0]); i++) {
        if (device_family[i].id[0] == id[0] &&
            device_family[i].id[1] == id[1] &&
            device_family[i].id[2] == id[2])
            return &device_family[i];
    }
    return NULL;
}

static const DeviceFamilyEntry *lookup_device_by_name(const char *name)
{
    for (size_t i = 0; i < sizeof(device_family)/sizeof(device_family[0]); i++) {
        if (strcasecmp(device_family[i].name, name) == 0)
            return &device_family[i];
    }
    return NULL;
}
MAYBE_STATIC void parse_args(int argc, char *argv[], AppConfig *cfg);
MAYBE_STATIC void event_loop(AppConfig *cfg, RspHandlers *h);
MAYBE_STATIC int  load_segments(AppConfig *cfg, ElfContext *ctx);
MAYBE_STATIC int  verify_segments(AppConfig *cfg, ElfContext *ctx);
MAYBE_STATIC int  run_device_mode(AppConfig *cfg);
MAYBE_STATIC int  run_prog_mode(AppConfig *cfg);
MAYBE_STATIC int  run_reset_mode(AppConfig *cfg);
MAYBE_STATIC void run_autobaud_probe(AppConfig *cfg);
MAYBE_STATIC void sig_handler(int signo);
MAYBE_STATIC void progress_render(const char *phase, const char *window,
                                  unsigned page, unsigned total);
MAYBE_STATIC void progress_finish(void);

MAYBE_STATIC void parse_args(int argc, char *argv[], AppConfig *cfg)
{
    cfg->serial_device = NULL;
    cfg->elf_path      = NULL;
    cfg->gdb_port      = 1234;
    cfg->baud_rate     = 115200;
    cfg->load_flash    = false;
    cfg->erase_chip    = false;
    cfg->device_info   = false;
    cfg->allow_lock_updi = false;
    cfg->prog_mode     = false;
    cfg->no_verify     = false;
    cfg->no_autobaud   = false;
    cfg->force_device  = NULL;
    cfg->allow_erase   = false;
    cfg->log_rsp       = false;
    cfg->no_introspect = false;
    cfg->reset_cpu      = false;
    cfg->dap_mode       = false;   /* default: RSP front-end */
    cfg->debug_in_sleep = true;    /* default: keep clock alive in SLEEP */
    cfg->listen_fd     = -1;
    cfg->gdb_fd        = -1;
    cfg->updi_fd       = -1;

    /* Phase 16: track explicit --rsp / --dap so the two can be rejected when
     * both are given.  cfg->dap_mode alone can't distinguish "default RSP"
     * from "--rsp given". */
    bool saw_rsp = false, saw_dap = false;
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
        } else if (strcmp(a, "--allow-lock-updi") == 0) {
            cfg->allow_lock_updi = true;
        } else if (strncmp(a, "--force-device=", 15) == 0) {
            cfg->force_device = a + 15;
            if (cfg->force_device[0] == '\0') {
                fprintf(stderr,
                        "%s: --force-device= requires a family name "
                        "(AVR-DA|AVR-DB|AVR-DD|AVR-DU|AVR-SD)\n", argv[0]);
                usage(argv[0]);
                exit(1);
            }
        } else if (strcmp(a, "--device") == 0) {
            cfg->device_info = true;
        } else if (strcmp(a, "--prog") == 0) {
            cfg->prog_mode = true;
        } else if (strcmp(a, "--no-verify") == 0) {
            cfg->no_verify = true;
        } else if (strcmp(a, "--no-autobaud") == 0) {
            cfg->no_autobaud = true;
        } else if (strcmp(a, "--allow-erase") == 0) {
            cfg->allow_erase = true;
        } else if (strcmp(a, "--log-rsp") == 0) {
            cfg->log_rsp = true;
        } else if (strcmp(a, "--no-introspect") == 0) {
            cfg->no_introspect = true;
        } else if (strcmp(a, "--reset") == 0) {
            cfg->reset_cpu = true;
        } else if (strcmp(a, "--sleep") == 0) {
            cfg->debug_in_sleep = false;
        } else if (strcmp(a, "--rsp") == 0) {
            cfg->dap_mode = false;
            saw_rsp = true;
        } else if (strcmp(a, "--dap") == 0) {
            cfg->dap_mode = true;
            saw_dap = true;
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

    /* Phase 16: --rsp and --dap are mutually exclusive. */
    if (saw_rsp && saw_dap) {
        fprintf(stderr,
                "%s: error: --rsp and --dap are mutually exclusive\n",
                argv[0]);
        usage(argv[0]);
        exit(1);
    }

    /* LLR-MAIN-08: --device and --load are mutually exclusive. */
    if (cfg->device_info && cfg->load_flash) {
        fprintf(stderr,
                "%s: error: --device is mutually exclusive with --load\n",
                argv[0]);
        usage(argv[0]);
        exit(1);
    }

    /* LLR-MAIN-15: --prog is mutually exclusive with --device and --load. */
    if (cfg->prog_mode && (cfg->device_info || cfg->load_flash)) {
        fprintf(stderr,
                "%s: error: --prog is mutually exclusive with --device and --load\n",
                argv[0]);
        usage(argv[0]);
        exit(1);
    }

    if (cfg->serial_device == NULL) {
        usage(argv[0]);
        exit(1);
    }
    /* --device makes <elf-file> optional (LLR-MAIN-08).
     * --reset makes <elf-file> optional.
     * --prog requires an ELF (LLR-MAIN-15).                            */
    if (!cfg->device_info && !cfg->reset_cpu && cfg->elf_path == NULL) {
        usage(argv[0]);
        exit(1);
    }
}

MAYBE_STATIC void sig_handler(int signo)
{
    /* HLR-065 / LLR-MAIN-22: record the signo so app_main() can name it
     * in the shutdown lifecycle line.  Only the first delivered signal
     * is captured — subsequent ones are ignored to keep the log honest. */
    if (g_shutdown_signal == 0) g_shutdown_signal = (sig_atomic_t)signo;
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

        /* (1) accept new GDB client when none is connected.
         * Halt the CPU on attach so the very first `?` / `g` reflects
         * a coherent stopped state.  After a prior session's detach
         * (which calls updi_run) the target is free-running, and the
         * subsequent vCont;c would fail because the OCD module sees
         * the RUN bit being set on an already-running CPU.            */
        if (cfg->listen_fd >= 0 && FD_ISSET(cfg->listen_fd, &rfds) &&
            cfg->gdb_fd < 0) {
            int fd = rsp_accept(cfg->listen_fd);
            if (fd >= 0) {
                cfg->gdb_fd = fd;
                if (cfg->updi_fd >= 0) (void)updi_halt(cfg->updi_fd);
                /* HLR-065 / LLR-MAIN-22: log peer address. */
                struct sockaddr_in peer;
                socklen_t plen = sizeof peer;
                if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0 &&
                    peer.sin_family == AF_INET) {
                    char ip[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
                    fprintf(stderr, "avrOSdb: client connected from %s:%u\n",
                            ip, (unsigned)ntohs(peer.sin_port));
                } else {
                    fprintf(stderr, "avrOSdb: client connected from <unknown>\n");
                }
                fflush(stderr);
            }
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
                /* HLR-065 / LLR-MAIN-22: classify peer-side disconnect. */
                if (rc == 0) {
                    fprintf(stderr, "avrOSdb: client disconnected (EOF)\n");
                } else {
                    fprintf(stderr, "avrOSdb: client disconnected (read error: %s)\n",
                            strerror(errno));
                }
                fflush(stderr);
                rsp_close(cfg->gdb_fd);
                cfg->gdb_fd = -1;
                /* Force fresh state for the next connection so a new client
                 * doesn't inherit a stale halt reason (like a ghost breakpoint). */
                if (h->ctx != NULL) {
                    ((RspContext *)h->ctx)->last_stop_cause = 0;
                }
            } else {
                (void)rsp_dispatch_n(cfg->gdb_fd, pkt, (size_t)rc, h);
                /* HLR-065 / LLR-RSP-43 / LLR-MAIN-22: drain any
                 * disconnect classification that the protocol layer
                 * recorded during dispatch (D, vKill, ...).            */
                RspContext *rctx = (RspContext *)h->ctx;
                if (rctx != NULL && rctx->disconnect_reason != NULL) {
                    fprintf(stderr, "avrOSdb: client disconnected (%s)\n",
                            rctx->disconnect_reason);
                    fflush(stderr);
                    rctx->disconnect_reason = NULL;
                    rctx->last_stop_cause = 0;
                    /* If the handler did not close the socket itself
                     * (e.g. vKill replied OK and set quit_p but left
                     * gdb_fd open), close it now so we don't double-log
                     * on the next event-loop iteration.                */
                    if (cfg->gdb_fd >= 0) {
                        rsp_close(cfg->gdb_fd);
                        cfg->gdb_fd = -1;
                    }
                }
            }
        }
    }
}

/* Iterate PT_LOAD segments and flash each non-SRAM segment.
 * Returns 0 on success, -1 on any read or flash error. */
/* ── Phase 9 — auto-baud probe visitor callback (LLR-MAIN-16) ─────────
 * Silent by default: the probe runs at the start of every mode but we
 * only want the single `baud=#####` line printed elsewhere.  Kept as a
 * no-op stub so updi_probe_baud()'s callback-required contract holds
 * and the function pointer is non-NULL for ld --wrap test stubs.    */
static void autobaud_report_cb(int baud, int errors, int samples, void *user)
{
    (void)baud; (void)errors; (void)samples; (void)user;
}

/* LLR-MAIN-16: shared auto-baud entry point.  Invoked unconditionally by
 * every mode (--device, --prog, and the GDB-server path) before the
 * first updi_open(), unless the operator passed --no-autobaud.  On
 * success the chosen rung overwrites cfg->baud_rate; on total failure
 * the configured baud is left in place with a warning.               */
MAYBE_STATIC void run_autobaud_probe(AppConfig *cfg)
{
    if (cfg->no_autobaud)
        return;

    int chosen = updi_probe_baud(cfg->serial_device, 8,
                                 autobaud_report_cb, NULL);
    if (chosen > 0) {
        cfg->baud_rate = chosen;
    } else {
        fprintf(stderr,
                "warning: auto-baud probe failed at every rung; "
                "falling back to configured baud %d\n",
                cfg->baud_rate);
        fflush(stderr);
    }
}

/* ── Phase 9 — progress bar (LLR-MAIN-15) ──────────────────────────────
 * On a TTY stdout, emit a carriage-return-anchored ANSI bar that
 * repaints in place.  When stdout is a pipe (e.g. CI), degrade to one
 * line per page so log captures stay sane.                            */
MAYBE_STATIC void progress_render(const char *phase, const char *window,
                                  unsigned page, unsigned total)
{
    if (total == 0) return;
    unsigned pct = (unsigned)((page * 100u) / total);
    if (isatty(STDOUT_FILENO)) {
        const unsigned bar = 30u;
        unsigned filled = (bar * page) / total;
        if (filled > bar) filled = bar;
        char buf[64];
        size_t i = 0;
        buf[i++] = '[';
        for (unsigned k = 0; k < bar; k++)
            buf[i++] = (k < filled) ? '#' : '.';
        buf[i++] = ']';
        buf[i]   = '\0';
        fprintf(stdout, "\r%s %3u%%  page %u/%u  %s %s   ",
                buf, pct, page, total, phase, window);
        fflush(stdout);
    } else {
        fprintf(stdout, "%s %s page %u/%u (%u%%)\n",
                phase, window, page, total, pct);
        fflush(stdout);
    }
}

MAYBE_STATIC void progress_finish(void)
{
    if (isatty(STDOUT_FILENO)) {
        fputc('\n', stdout);
        fflush(stdout);
    }
}

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

    /* LLR-MAIN-16 / LLR-UPDI-31: probe the candidate-baud ladder before
     * opening the link.  Centralised in run_autobaud_probe(); no-op when
     * --no-autobaud was passed.                                         */
    run_autobaud_probe(cfg);

    fd = updi_open(cfg->serial_device, cfg->baud_rate);
    if (fd < 0) {
        fprintf(stderr,
                "error: updi-open failed for '%s' \u2014 "
                "check wiring, target power, UPDIDIS fuse\n",
                cfg->serial_device);
        return 1;
    }
    cfg->updi_fd = fd;

    /* Select the per-family memory map (HLR-046).  In diagnostic mode
     * we apply only the explicit override; autodetect is left to the
     * subsequent `updi_read_device_info()` call so the report shows
     * the raw SIGROW bytes even on unknown silicon.                    */
    if (cfg->force_device != NULL && cfg->force_device[0] != '\0') {
        if (updi_select_device(fd, cfg->force_device) < 0) {
            updi_close(fd);
            cfg->updi_fd = -1;
            return 1;
        }
    }

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
    const DeviceFamilyEntry *e = lookup_device_by_id(info.device_id);
    printf("Family:          %s\n", e ? e->name : "unknown device");
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

    /* LLR-MAIN-17 / LLR-UPDI-32: read FUSES + LOCK windows and render a
     * human-readable pretty-print.  Soft-fail: if the read fails we
     * just skip the section so a damaged device still reports SIGROW. */
    {
        const UpdiDeviceMap *dev = updi_get_device();
        if (dev != NULL && dev->fuses_size > 0 && dev->fuses_size <= 64) {
            uint8_t raw[64];
            uint8_t lock[4];
            int     have_lock = 0;
            if (updi_nvm_read(fd, dev->fuses_base, raw, dev->fuses_size) == 0) {
                if (dev->lock_base != 0 &&
                    updi_nvm_read(fd, dev->lock_base, lock, sizeof lock) == 0)
                    have_lock = 1;
                char text[2048];
                int n = updi_format_fuses(text, sizeof text,
                                          raw,  dev->fuses_size,
                                          have_lock ? lock : NULL,
                                          have_lock ? sizeof lock : 0);
                if (n > 0)
                    fputs(text, stdout);
            } else {
                fprintf(stderr,
                        "warning: FUSES read failed; skipping fuse decode\n");
            }
            fflush(stdout);
        }
    }

    updi_close(fd);
    cfg->updi_fd = -1;
    return 0;
}

/* run_reset_mode: open the UPDI link and immediately close it.
 * updi_close() pulses ASI_RESET_REQ (assert then release) before
 * tearing down the serial link, which issues a clean system reset to
 * the target CPU.  No ELF, no GDB listener, no event loop.          */
MAYBE_STATIC int run_reset_mode(AppConfig *cfg)
{
    int fd = updi_open(cfg->serial_device, cfg->baud_rate);
    if (fd < 0) {
        fprintf(stderr,
                "error: updi-open failed for '%s' — "
                "check wiring, target power, UPDIDIS fuse\n",
                cfg->serial_device);
        return 1;
    }
    cfg->updi_fd = fd;
    updi_close(fd);
    cfg->updi_fd = -1;
    fprintf(stdout, "reset: OK\n");
    fflush(stdout);
    return 0;
}

/* Iterate PT_LOAD segments and program each one to the NVM window that
 * its UPDI address falls within (Phase 8, LLR-MAIN-11).
 *
 * avr-gcc / avr-libc emit non-FLASH sections at "ELF VMA bands" that are
 * a toolchain convention, NOT silicon UPDI addresses.  This function
 * translates each `p_vaddr` to the silicon address NVMCTRL decodes:
 *
 *   ELF VMA band              UPDI silicon window     writer
 *   ─────────────────────     ─────────────────────   ─────────────────
 *   0x000000..0x01FFFF        0x800000+offset (FLASH) updi_nvm_write_flash
 *   0x810000+offset           0x1400+offset  (EEPROM) updi_nvm_write_eeprom
 *   0x820000+offset           0x1050+offset  (FUSES)  updi_nvm_write_fuses
 *   0x830000+offset           0x1040+offset  (LOCK)   updi_nvm_write_lockbits
 *   0x840000+offset           0x1100+offset  (SIGROW) logged & skipped (RO)
 *   0x850000+offset           0x1080+offset  (USERROW)updi_nvm_write_userrow
 *   sram_base..sram_base+sz   —                       silently skipped
 *
 * Returns 0 on success, -1 on any read or programming error. */

/* ── Unified write/verify progress tracking ──────────────────────────
 * load_segments() and verify_segments() each render a single progress
 * bar that spans every PT_LOAD segment.  Progress is counted in 256 B
 * "chunks" so FLASH (programmed 512 B per page) and non-FLASH (one
 * byte per STS) share a common denominator; one FLASH page commit
 * advances the counter by 2.  The bar is rendered via progress_render()
 * which already handles the TTY-vs-pipe degradation.                */
#define PROGRESS_CHUNK 256u

static unsigned s_load_done_pages;
static unsigned s_load_total_pages;
static const char *s_load_window;

/* Pre-scan the ELF and compute the total PROGRESS_CHUNK count we are
 * going to program (across all non-SIGROW PT_LOAD segments).         */
static int load_count_total_pages(ElfContext *ctx, unsigned *out_total)
{
    Elf32_Ehdr ehdr = ctx->ehdr;
    unsigned total = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        off_t off = (off_t)ehdr.e_phoff +
                    (off_t)i * (off_t)sizeof(Elf32_Phdr);
        if (lseek(ctx->fd, off, SEEK_SET) < 0) return -1;
        if (read(ctx->fd, &ph, sizeof ph) != (ssize_t)sizeof ph) return -1;
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        if (ctx->sram_base != 0 && ph.p_paddr >= ctx->sram_base) continue;

        if (ph.p_paddr < UPDI_FLASH_BASE) {
            /* FLASH: pad to page boundary first. */
            uint32_t padded = (ph.p_filesz + UPDI_FLASH_PAGE_SIZE - 1u) &
                              ~(UPDI_FLASH_PAGE_SIZE - 1u);
            total += (padded + PROGRESS_CHUNK - 1u) / PROGRESS_CHUNK;
        } else {
            uint32_t band = ph.p_paddr & ELF_VMA_BAND_MASK;
            if (band == ELF_VMA_SIGROW) continue;   /* skipped */
            total += ((unsigned)ph.p_filesz + PROGRESS_CHUNK - 1u)
                     / PROGRESS_CHUNK;
        }
    }
    *out_total = total;
    return 0;
}

/* Callback invoked from updi_nvm_write_flash() after each FLASH page
 * commit.  Maps the per-call `done` bytes to the global page counter
 * and repaints the bar.                                              */
static void load_flash_progress_cb(uint32_t addr, size_t done, size_t total,
                                   void *user)
{
    (void)addr; (void)user;
    /* `done` is cumulative within this updi_nvm_write_flash() call;
     * the caller has stashed the pre-segment cumulative count in
     * s_load_done_pages.  Re-render using that base + done/CHUNK.    */
    static unsigned seg_base_pages;
    if (done == UPDI_FLASH_PAGE_SIZE) {
        /* First page of a fresh segment — snapshot the base. */
        seg_base_pages = s_load_done_pages;
    }
    unsigned cur = seg_base_pages + (unsigned)(done / PROGRESS_CHUNK);
    progress_render("programming", s_load_window,
                    cur, s_load_total_pages);
    if (done == total) {
        s_load_done_pages = cur;     /* commit at end of segment */
    }
}

MAYBE_STATIC int load_segments(AppConfig *cfg, ElfContext *ctx)
{
    Elf32_Ehdr ehdr = ctx->ehdr;
    const UpdiDeviceMap *dev = updi_get_device();

    s_load_done_pages  = 0;
    s_load_total_pages = 0;
    s_load_window      = "FLASH";
    if (load_count_total_pages(ctx, &s_load_total_pages) < 0) return -1;
    updi_set_nvm_progress(load_flash_progress_cb, NULL);

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        off_t off = (off_t)ehdr.e_phoff +
                    (off_t)i * (off_t)sizeof(Elf32_Phdr);
        if (lseek(ctx->fd, off, SEEK_SET) < 0) return -1;
        if (read(ctx->fd, &ph, sizeof ph) != (ssize_t)sizeof ph) return -1;
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;

        /* SRAM segments (when an SRAM image is present in the ELF) are
         * skipped silently. */
        if (ctx->sram_base != 0 &&
            ph.p_paddr >= ctx->sram_base) {
            continue;
        }

        /* Translate ELF VMA → silicon UPDI address.  FLASH uses p_vaddr
         * starting at 0 and is rebased into the 0x800000 window.  Every
         * other band is mapped to its silicon UPDI_*_BASE.              */
        uint32_t updi_addr;
        bool in_flash = false, in_eeprom = false, in_userrow = false;
        bool in_fuses = false, in_lock = false, in_sigrow = false;

        if (ph.p_paddr < UPDI_FLASH_BASE) {
            updi_addr = ph.p_paddr | UPDI_FLASH_BASE;
            in_flash = true;
        } else {
            uint32_t band = ph.p_paddr & ELF_VMA_BAND_MASK;
            uint32_t lo   = ph.p_paddr & ELF_VMA_OFFSET_MASK;
            switch (band) {
            case ELF_VMA_EEPROM:
                updi_addr = dev->eeprom_base + lo;  in_eeprom  = true; break;
            case ELF_VMA_USERROW:
                updi_addr = dev->userrow_base + lo; in_userrow = true; break;
            case ELF_VMA_FUSES:
                updi_addr = dev->fuses_base + lo;   in_fuses   = true; break;
            case ELF_VMA_LOCK:
                updi_addr = dev->lock_base + lo;    in_lock    = true; break;
            case ELF_VMA_SIGROW:
                updi_addr = dev->sigrow_base + lo;  in_sigrow  = true; break;
            default:
                fprintf(stderr,
                        "error: segment vaddr 0x%06x not in any programmable "
                        "NVM window\n", (unsigned)ph.p_paddr);
                return -1;
            }
        }

        /* SIGROW: read-only, log and skip. */
        if (in_sigrow) {
            fprintf(stdout, "SIGROW segment @0x%06x ignored (read-only)\n",
                    (unsigned)updi_addr);
            continue;
        }

        /* FLASH window: handle arbitrary byte alignments via read-modify-write. */
        if (in_flash) {
            s_load_window = "FLASH";
            uint8_t *buf = malloc(ph.p_filesz);
            if (!buf && ph.p_filesz > 0) { updi_set_nvm_progress(NULL, NULL); return -1; }
            if (lseek(ctx->fd, (off_t)ph.p_offset, SEEK_SET) < 0 ||
                read(ctx->fd, buf, ph.p_filesz) != (ssize_t)ph.p_filesz) {
                free(buf);
                updi_set_nvm_progress(NULL, NULL);
                return -1;
            }
            int rc = updi_nvm_flash_patch(cfg->updi_fd, updi_addr, buf, ph.p_filesz);
            free(buf);
            if (rc < 0) { updi_set_nvm_progress(NULL, NULL); return -1; }
            /* Note: progress bar will not update smoothly for small patches */
            uint32_t padded = (ph.p_filesz + UPDI_FLASH_PAGE_SIZE - 1u) & ~(UPDI_FLASH_PAGE_SIZE - 1u);
            s_load_done_pages += padded / UPDI_FLASH_PAGE_SIZE;
            continue;
        }

        /* Classify against non-FLASH windows.  Each branch reads the
         * exact segment payload (no padding) and dispatches to the
         * matching NVM routine.                                       */
        uint32_t base = 0, size = 0;
        const char *kind = NULL;
        int (*writer)(int, uint32_t, const uint8_t *, size_t) = NULL;
        bool is_lockbits = false;

        if (in_eeprom) {
            base = dev->eeprom_base; size = dev->eeprom_size;
            kind = "EEPROM"; writer = updi_nvm_write_eeprom;
        } else if (in_userrow) {
            base = dev->userrow_base; size = dev->userrow_size;
            kind = "USERROW"; writer = updi_nvm_write_userrow;
        } else if (in_fuses) {
            base = dev->fuses_base; size = dev->fuses_size;
            kind = "FUSES"; writer = updi_nvm_write_fuses;
        } else if (in_lock) {
            base = dev->lock_base; size = dev->lock_size;
            kind = "LOCK"; is_lockbits = true;
        } else {
            fprintf(stderr,
                    "error: segment vaddr 0x%06x not in any programmable "
                    "NVM window\n", (unsigned)updi_addr);
            return -1;
        }

        if ((updi_addr - base) + ph.p_filesz > size) {
            fprintf(stderr,
                    "error: %s segment @0x%06x len=%u exceeds window size %u\n",
                    kind, (unsigned)updi_addr,
                    (unsigned)ph.p_filesz, (unsigned)size);
            return -1;
        }

        /* LOCK segments require a prior --erase so LOCKSTATUS is clear. */
        if (is_lockbits && !cfg->erase_chip) {
            fprintf(stderr,
                    "error: LOCK segment present but --erase was not supplied; "
                    "lockbits can only be written after a chip-erase\n");
            return -1;
        }

        uint8_t *buf = malloc(ph.p_filesz);
        if (!buf) return -1;
        if (lseek(ctx->fd, (off_t)ph.p_offset, SEEK_SET) < 0 ||
            read(ctx->fd, buf, ph.p_filesz) != (ssize_t)ph.p_filesz) {
            free(buf);
            return -1;
        }
        int rc;
        if (is_lockbits) {
            rc = updi_nvm_write_lockbits(cfg->updi_fd, updi_addr, buf,
                                         ph.p_filesz, cfg->allow_lock_updi);
        } else {
            rc = writer(cfg->updi_fd, updi_addr, buf, ph.p_filesz);
        }
        free(buf);
        if (rc < 0) {
            fprintf(stderr, "error: %s programming failed (rc=%d)\n",
                    kind, rc);
            updi_set_nvm_progress(NULL, NULL);
            return -1;
        }
        /* Non-FLASH writers are atomic per segment; advance the unified
         * progress counter by ceil(filesz / PROGRESS_CHUNK) in one step. */
        s_load_window = kind;
        s_load_done_pages += ((unsigned)ph.p_filesz + PROGRESS_CHUNK - 1u)
                             / PROGRESS_CHUNK;
        progress_render("programming", kind,
                        s_load_done_pages, s_load_total_pages);
    }
    progress_finish();
    updi_set_nvm_progress(NULL, NULL);
    return 0;
}

/* ── Phase 9 — read-back verify (LLR-MAIN-14, LLR-UPDI-30) ────────────
 * Walks the same PT_LOAD segment classification load_segments() uses,
 * but instead of writing, reads each silicon byte back via
 * updi_nvm_read() and compares the page-CRC32 against the ELF payload.
 * On mismatch, prints the offending window + page offset + expected
 * and actual CRCs, then continues to the next segment so the operator
 * gets a complete picture in a single run.
 *
 * Returns 0 on full match, -1 on any verify failure or read error.   */
MAYBE_STATIC int verify_segments(AppConfig *cfg, ElfContext *ctx)
{
    Elf32_Ehdr ehdr = ctx->ehdr;
    const UpdiDeviceMap *dev = updi_get_device();
    const size_t CHUNK = PROGRESS_CHUNK;
    int rc_overall = 0;

    /* Defensive: discard the first FLASH read after the program phase.
     * Even with NVMCTRL.CTRLA cleared to NOCMD inside updi_nvm_write_flash,
     * the very first LDS burst into the FLASH window can return stale
     * page-buffer data on AVR-Dx silicon.  A throw-away 1-byte read
     * flushes the controller before the CRC compare begins.            */
    {
        uint8_t dummy;
        (void)updi_nvm_read(cfg->updi_fd, UPDI_FLASH_BASE, &dummy, 1);
    }

    /* Pre-scan: total CHUNK count across every verifiable segment. */
    unsigned total_pages = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        off_t off = (off_t)ehdr.e_phoff +
                    (off_t)i * (off_t)sizeof(Elf32_Phdr);
        if (lseek(ctx->fd, off, SEEK_SET) < 0) return -1;
        if (read(ctx->fd, &ph, sizeof ph) != (ssize_t)sizeof ph) return -1;
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        if (ctx->sram_base != 0 && ph.p_paddr >= ctx->sram_base) continue;
        if (ph.p_paddr >= UPDI_FLASH_BASE) {
            uint32_t band = ph.p_paddr & ELF_VMA_BAND_MASK;
            if (band == ELF_VMA_LOCK || band == ELF_VMA_SIGROW) continue;
        }
        total_pages += ((unsigned)ph.p_filesz + (unsigned)CHUNK - 1u)
                       / (unsigned)CHUNK;
    }

    unsigned done_pages = 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr ph;
        off_t off = (off_t)ehdr.e_phoff +
                    (off_t)i * (off_t)sizeof(Elf32_Phdr);
        if (lseek(ctx->fd, off, SEEK_SET) < 0) return -1;
        if (read(ctx->fd, &ph, sizeof ph) != (ssize_t)sizeof ph) return -1;
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;

        if (ctx->sram_base != 0 && ph.p_paddr >= ctx->sram_base) continue;

        uint32_t updi_addr;
        const char *kind = NULL;
        bool skip = false;

        if (ph.p_paddr < UPDI_FLASH_BASE) {
            updi_addr = ph.p_paddr | UPDI_FLASH_BASE;
            kind = "FLASH";
        } else {
            uint32_t band = ph.p_paddr & ELF_VMA_BAND_MASK;
            uint32_t lo   = ph.p_paddr & ELF_VMA_OFFSET_MASK;
            switch (band) {
            case ELF_VMA_EEPROM:
                updi_addr = dev->eeprom_base + lo;  kind = "EEPROM"; break;
            case ELF_VMA_USERROW:
                updi_addr = dev->userrow_base + lo; kind = "USERROW"; break;
            case ELF_VMA_FUSES:
                updi_addr = dev->fuses_base + lo;   kind = "FUSES"; break;
            case ELF_VMA_LOCK:
                /* LOCK contents are not stable for read-back compare;
                 * write succeeded if the chip is still talking.        */
                skip = true; updi_addr = 0; break;
            case ELF_VMA_SIGROW:
                skip = true; updi_addr = 0; break;
            default:
                fprintf(stderr,
                        "verify: segment vaddr 0x%06x not in any NVM window\n",
                        (unsigned)ph.p_paddr);
                return -1;
            }
        }
        if (skip) continue;

        uint8_t *expect = malloc(ph.p_filesz);
        if (!expect) return -1;
        if (lseek(ctx->fd, (off_t)ph.p_offset, SEEK_SET) < 0 ||
            read(ctx->fd, expect, ph.p_filesz) != (ssize_t)ph.p_filesz) {
            free(expect);
            return -1;
        }

        unsigned pages = (unsigned)((ph.p_filesz + CHUNK - 1u) / CHUNK);
        uint8_t actual[CHUNK];

        for (unsigned p = 0; p < pages; p++) {
            size_t pos = (size_t)p * CHUNK;
            size_t n   = (ph.p_filesz - pos) < CHUNK ? (ph.p_filesz - pos)
                                                    : CHUNK;
            if (updi_nvm_read(cfg->updi_fd,
                              updi_addr + (uint32_t)pos,
                              actual, n) < 0) {
                fprintf(stderr,
                        "\nverify: read failed @ 0x%06x len=%zu\n",
                        (unsigned)(updi_addr + (uint32_t)pos), n);
                free(expect);
                return -1;
            }
            if (memcmp(actual, expect + pos, n) != 0) {
                uint32_t exp_crc = updi_crc32(expect + pos, n);
                uint32_t act_crc = updi_crc32(actual, n);
                fprintf(stderr,
                        "\nverify: mismatch %s @ 0x%06x len=%zu "
                        "expected-crc=0x%08X actual-crc=0x%08X\n",
                        kind, (unsigned)(updi_addr + (uint32_t)pos), n,
                        (unsigned)exp_crc, (unsigned)act_crc);
                rc_overall = -1;
            }
            done_pages++;
            progress_render("verifying", kind, done_pages, total_pages);
        }
        free(expect);
    }
    progress_finish();
    return rc_overall;
}

/* run_prog_mode (LLR-MAIN-15): program-and-exit pipeline.  Opens UPDI,
 * optionally erases, loads, verifies (unless --no-verify), and returns
 * 0 / 1 / UPDI_EXIT_VERIFY_FAIL.  Never starts the GDB listener or the
 * event loop.                                                          */
MAYBE_STATIC int run_prog_mode(AppConfig *cfg)
{
    ElfContext elf_ctx;
    memset(&elf_ctx, 0, sizeof elf_ctx);
    elf_ctx.fd = -1;

    if (elf_open(cfg->elf_path, &elf_ctx) < 0) {
        fprintf(stderr, "error: failed to open ELF '%s'\n", cfg->elf_path);
        return 1;
    }

    /* LLR-MAIN-16: probe the baud ladder before opening UPDI. */
    run_autobaud_probe(cfg);

    cfg->updi_fd = updi_open(cfg->serial_device, cfg->baud_rate);
    if (cfg->updi_fd < 0) {
        fprintf(stderr, "error: updi-open failed for '%s'\n",
                cfg->serial_device);
        elf_close(&elf_ctx);
        return 1;
    }

    fprintf(stdout, "baud=%d\n", cfg->baud_rate);
    fflush(stdout);

    /* LLR-MAIN-13: identify the per-family memory map before any NVM
     * write touches USERROW/EEPROM/FUSES/LOCK windows.  Unconditional
     * SIGROW autodetect (unless --force-device overrides) so the
     * ELF↔silicon family cross-check below sees an unbiased result. */
    const char *elf_family =
        updi_family_from_partname(elf_ctx.device_name);
    if (updi_select_device(cfg->updi_fd, cfg->force_device) < 0) {
        updi_close(cfg->updi_fd); cfg->updi_fd = -1;
        elf_close(&elf_ctx);
        return 1;
    }

    /* LLR-MAIN-13: ELF↔silicon family mismatch guard.  When the ELF
     * names a known family and the user did NOT supply --force-device,
     * the autodetected family must match the ELF-declared family or we
     * abort — programming FUSES/EEPROM/USERROW/LOCK at the wrong base
     * for the silicon actually attached silently corrupts non-FLASH
     * NVM.  --force-device is the explicit escape hatch.              */
    if (elf_family != NULL
        && (cfg->force_device == NULL || cfg->force_device[0] == '\0')) {
        const char *active = updi_get_device()->family;
        if (active == NULL || strcmp(active, elf_family) != 0) {
            fprintf(stderr,
                    "error: ELF was built for %s (family %s) but target "
                    "reports %s (use --force-device=%s to override)\n",
                    elf_ctx.device_name, elf_family,
                    (active != NULL ? active : "?"),
                    (active != NULL ? active : elf_family));
            updi_close(cfg->updi_fd); cfg->updi_fd = -1;
            elf_close(&elf_ctx);
            return 1;
        }
    }

    if (cfg->erase_chip) {
        if (updi_chip_erase(cfg->updi_fd) < 0) {
            fprintf(stderr, "error: chip erase failed\n");
            updi_close(cfg->updi_fd); cfg->updi_fd = -1;
            elf_close(&elf_ctx);
            return 1;
        }
    }

    if (load_segments(cfg, &elf_ctx) < 0) {
        updi_close(cfg->updi_fd); cfg->updi_fd = -1;
        elf_close(&elf_ctx);
        return 1;
    }

    int verify_rc = 0;
    if (!cfg->no_verify) {
        verify_rc = verify_segments(cfg, &elf_ctx);
    }

    updi_close(cfg->updi_fd); cfg->updi_fd = -1;
    elf_close(&elf_ctx);

    if (verify_rc < 0) {
        fprintf(stderr, "verify: FAILED\n");
        return UPDI_EXIT_VERIFY_FAIL;
    }
    if (!cfg->no_verify) {
        fprintf(stdout, "verify: OK\n");
        fflush(stdout);
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

    /* LLR-MAIN-15: --prog short-circuits to the program+verify+exit
     * pipeline.  No GDB listener, no event loop. */
    if (cfg.prog_mode) {
        return run_prog_mode(&cfg);
    }

    /* --reset: pulse UPDI system reset and exit. */
    if (cfg.reset_cpu) {
        return run_reset_mode(&cfg);
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

    /* LLR-MAIN-16: probe the baud ladder before opening UPDI. */
    run_autobaud_probe(&cfg);

    /* LLR-MAIN-03: open UPDI first; verify before listening. */
    cfg.updi_fd = updi_open(cfg.serial_device, cfg.baud_rate);
    if (cfg.updi_fd < 0) {
        fprintf(stderr, "error: cannot open UPDI device '%s'\n",
                cfg.serial_device);
        exit_code = 1;
        goto teardown;
    }

    /* HLR-046 / LLR-MAIN-13: identify the per-family memory map
     * before any NVM write touches USERROW/EEPROM/FUSES/LOCK windows.
     * Precedence:
     *   1. --force-device=<family>  — user override, no autodetect.
     *   2. SIGROW autodetect        — runs whenever --force-device is
     *      not set, regardless of whether the ELF carries deviceinfo.
     * The ELF `.note.gnu.avr.deviceinfo` part-name is NOT used to
     * select the family; using it as a selection input would defeat
     * the mismatch check below (we'd be comparing the ELF family
     * against the family we just told the UPDI layer to be).  Instead
     * the ELF-derived family acts purely as an assertion: SIGROW
     * autodetect runs unbiased, then the result is cross-checked
     * against the ELF.                                                */
    const char *elf_family =
        updi_family_from_partname(elf_ctx.device_name);
    if (updi_select_device(cfg.updi_fd, cfg.force_device) < 0) {
        exit_code = 1;
        goto teardown;
    }

    /* LLR-MAIN-13: ELF↔silicon family mismatch guard.  When the ELF
     * names a known family and the user did NOT supply --force-device,
     * the family that updi_select_device() ended up on must match the
     * ELF-declared family.  Mismatch would let load_segments() write
     * EEPROM/USERROW/FUSE/LOCK at the wrong bases/sizes for the silicon
     * actually attached, which silently corrupts non-FLASH NVM.  The
     * --force-device escape hatch lets the user override on purpose.  */
    if (elf_family != NULL
        && (cfg.force_device == NULL || cfg.force_device[0] == '\0')) {
        const char *active = updi_get_device()->family;
        if (active == NULL || strcmp(active, elf_family) != 0) {
            fprintf(stderr,
                    "error: ELF was built for %s (family %s) but target "
                    "reports %s (use --force-device=%s to override)\n",
                    elf_ctx.device_name, elf_family,
                    (active != NULL ? active : "?"),
                    (active != NULL ? active : elf_family));
            exit_code = 1;
            goto teardown;
        }
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
        if (load_segments(&cfg, &elf_ctx) < 0) {
            fprintf(stderr, "error: load failed\n");
            exit_code = 1;
            goto teardown;
        }
        /* LLR-MAIN-14: read-back verify after --load (unless suppressed). */
        if (!cfg.no_verify) {
            int vrc = verify_segments(&cfg, &elf_ctx);
            if (vrc < 0) {
                fprintf(stderr, "verify: FAILED\n");
                exit_code = UPDI_EXIT_VERIFY_FAIL;
                goto teardown;
            }
            fprintf(stdout, "verify: OK\n");
            fflush(stdout);
        }
    }

    /* Transition target into OCD (debug) mode before accepting GDB
     * connections.  This sends the 'OCD     ' key + a reset pulse; the
     * CPU halts at the reset vector (SOR_DIS=0 by default), giving GDB
     * a clean stopped state to attach to.  Any NVMPROG state left over
     * from --erase / --load is dropped by the reset.                  */
    /* Phase 20: select the debug-in-sleep clock policy before entering OCD
     * (and for every later re-entry, e.g. monitor reset). Default on; --sleep
     * disables it to keep native target sleep/power behaviour. */
    updi_set_debug_in_sleep(cfg.debug_in_sleep);
    if (updi_enter_debug(cfg.updi_fd) < 0) {
        fprintf(stderr, "error: failed to enter OCD debug mode\n");
        exit_code = 1;
        goto teardown;
    }

    cfg.listen_fd = rsp_listen(cfg.gdb_port);
    if (cfg.listen_fd < 0) {
        fprintf(stderr, "error: cannot bind GDB listener on port %u\n",
                (unsigned)cfg.gdb_port);
        exit_code = 1;
        goto teardown;
    }
    /* HLR-065 / LLR-MAIN-22: announce server-ready state. */
    fprintf(stderr, "avrOSdb: listening on :%u\n", (unsigned)cfg.gdb_port);
    fflush(stderr);

    /* Best-effort session init; synthetic threads are enabled only
     * when the ELF exposes avrOS FSM table symbols and the user did
     * not explicitly disable them. */
    bool have_fsm_symbols = false;
    if (elf_find_avros_tables(&elf_ctx, &idx) == 0) {
        have_fsm_symbols = (elf_has_fsm_symbols(&idx) != 0);
    }
    bool enable_introspect = !cfg.no_introspect && have_fsm_symbols;
    if (enable_introspect) {
        (void)fsm_build_thread_list(&fsm_ctx, &idx, cfg.updi_fd);
    }

    UpdiDeviceInfo target_info;
    bool target_info_valid = false;
    if (updi_read_device_info(cfg.updi_fd, &target_info) == 0) {
        target_info_valid = true;
    }

    const DeviceFamilyEntry *dev_match = NULL;
    if (cfg.force_device != NULL) {
        dev_match = lookup_device_by_name(cfg.force_device);
    } else if (elf_ctx.device_name[0] != '\0') {
        dev_match = lookup_device_by_name(elf_ctx.device_name);
    }
    if (dev_match == NULL && target_info_valid) {
        dev_match = lookup_device_by_id(target_info.device_id);
    }

    DeviceMemoryLayout final_map;
    if (dev_match) {
        final_map = dev_match->map;
    } else {
        memset(&final_map, 0, sizeof final_map);
        final_map.flash_base = 0;
        final_map.flash_size = elf_ctx.flash_size;
        final_map.sram_base  = elf_ctx.sram_base;
        final_map.sram_size  = elf_ctx.sram_size;
    }

    int g_thread = -1, c_thread = -1;
    RspContext rctx = {
        .updi_fd    = cfg.updi_fd,
        .gdb_fd_p   = &cfg.gdb_fd,
        .fsm        = enable_introspect ? &fsm_ctx : NULL,
        .idx        = &idx,
        .g_thread_p = &g_thread,
        .c_thread_p = &c_thread,
        .quit_p     = &g_quit,
        .allow_erase = cfg.allow_erase ? 1 : 0,
        .bp_mode    = RSP_BP_MODE_SW,
                /* HLR-063: feed the target-detected memory layout to the
         * qXfer:memory-map:read handler.  Zero values disable the
         * map advertisement entirely.                                */
        .map        = final_map,
    };
    RspHandlers handlers;
    rsp_default_handlers(&handlers, &rctx);

    rsp_set_logging(cfg.log_rsp);
    /* Phase 16: dispatch to the selected protocol front-end. Both serve over
     * the same TCP listener + debug core; --rsp is the default. */
    if (cfg.dap_mode)
        dap_serve(cfg.listen_fd, cfg.updi_fd, &elf_ctx, &idx,
                  enable_introspect ? &fsm_ctx : NULL, &g_quit, cfg.log_rsp);
    else
        event_loop(&cfg, &handlers);

    /* HLR-065 / LLR-MAIN-22: announce shutdown reason just before
     * teardown frees the listener and silicon resources.              */
    {
        const char *reason;
        switch (g_shutdown_signal) {
            case SIGINT:  reason = "SIGINT";  break;
            case SIGTERM: reason = "SIGTERM"; break;
            case 0:       reason = "fatal: event loop exited"; break;
            default:      reason = "fatal: unknown"; break;
        }
        fprintf(stderr, "avrOSdb: shutting down (%s)\n", reason);
        fflush(stderr);
    }

teardown:
    /* LLR-MAIN-07: gdb_fd → listen_fd → elf_close → updi_close. */
    if (cfg.gdb_fd >= 0)    rsp_close(cfg.gdb_fd);
    if (cfg.listen_fd >= 0) rsp_close(cfg.listen_fd);
    if (elf_opened)         elf_close(&elf_ctx);
    if (cfg.updi_fd >= 0)   updi_close(cfg.updi_fd);
    return exit_code;
}
