/* src/updi.c — UPDI physical-layer implementation (Phase G: spec-aligned) */
#include "updi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ── UPDI opcode constants (datasheet §35.3.3, Fig. 35-6) ─────────────── */
#define UPDI_OP_LDS_24_8     0x08u   /* LDS  addr=24b, data= 8b               */
#define UPDI_OP_STS_24_8     0x48u   /* STS  addr=24b, data= 8b               */
#define UPDI_OP_ST_PTR_WORD  0x69u   /* ST  pointer-reg, addr size = word (16b) */
#define UPDI_OP_ST_PTR_LONG  0x6Au   /* ST  pointer-reg, addr size = long (24b) */
#define UPDI_OP_LD_PTR_INC   0x24u   /* LD  rd, *(ptr++)  size B = byte      */
#define UPDI_OP_ST_PTR_INC   0x64u   /* ST  *(ptr++), rr  size B = byte      */
#define UPDI_OP_ST_PTR_INC_W 0x65u   /* ST  *(ptr++), rr  size W = word      */
#define UPDI_OP_REPEAT       0xA0u   /* REPEAT count      size B = byte      */
#define UPDI_OP_LDCS_PREFIX  0x80u   /* LDCS cs : 0x80 | cs                  */
#define UPDI_OP_STCS_PREFIX  0xC0u   /* STCS cs : 0xC0 | cs                  */
#define UPDI_OP_KEY          0xE0u   /* KEY (64-bit key payload)             */

/* CTRLA values used during bulk page-buffer programming (matches avrdude).  *
 *   RSD_ON:  bit3 RSD=1 (Response Signature Disable) + GTVAL=6              *
 *   RSD_OFF: bit3 RSD=0                              + GTVAL=6              */
#define UPDI_CTRLA_RSD_ON    0x0Eu
#define UPDI_CTRLA_RSD_OFF   0x06u

/* ── ASI control / status bit positions (datasheet §35.5.9) ────────────── */
#define ASI_SYS_STATUS_NVMPROG 0x08u /* bit 3 — NVM programming active       */
#define ASI_SYS_STATUS_UROWPROG 0x04u/* bit 2 — USER_ROW programming active  */
/* NB: §35.5.9 defines no halt/stopped bit in ASI_SYS_STATUS. CPU halt /
 *     step / run live in the OCD register space (separate spec) and are
 *     deferred to Phase 3.                                                 */

/* ── ASI_KEY_STATUS bits (datasheet §35.5.5) ───────────────────────────── */
#define ASI_KEY_STATUS_CHIPER    0x08u /* bit 3 — Chip-Erase key latched     */
#define ASI_KEY_STATUS_NVMPROG   0x10u /* bit 4 — NVMProg     key latched    */
#define ASI_KEY_STATUS_UROWWRITE 0x20u /* bit 5 — UserRow     key latched    */

/* ── ASI_SYS_CTRLA bits (datasheet §35.5.8) ────────────────────────────── */
#define ASI_SYS_CTRLA_CLKREQ     0x01u /* bit 0 — request system clock       */
#define ASI_SYS_CTRLA_UROWDONE   0x02u /* bit 1 — commit USER_ROW write      */

/* ── ASI_RESET_REQ values (datasheet §35.5.6) ──────────────────────────── */
#define ASI_RESET_REQ_RUN    0x00u   /* clear reset                          */
#define ASI_RESET_REQ_RESET  0x59u   /* assert system reset                  */

/* ── NVMCTRL register map / bits / sizing (AVR-Dx, per Atmel.AVR-Dx_DFP) ── */
#define NVMCTRL_CTRLA        0x1000u
#define NVMCTRL_STATUS       0x1002u
#define NVMCTRL_CMD_NOCMD    0x00u   /* clear pending command                */
#define NVMCTRL_CMD_FLWR     0x02u   /* program page buffer to FLASH         */
#define NVMCTRL_CMD_FLPER    0x08u   /* erase one FLASH page                 */
#define NVMCTRL_CMD_EEERWR   0x13u   /* EEPROM/USERROW/FUSE byte erase+write */
#define NVMCTRL_CMD_CHER     0x20u   /* chip erase (full FLASH)              */
#define NVMCTRL_STATUS_FBUSY 0x01u   /* bit 0 = FLASH busy                   */
#define NVMCTRL_STATUS_EEBUSY 0x02u  /* bit 1 = EEPROM busy                  */

/* UPDI_FLASH_PAGE_SIZE is declared in updi.h */

/* ── NVM write progress callback (optional, opt-in via setter) ────────
 * File-scope so updi_nvm_write_flash() can invoke it from inside its
 * page-program loop.  Default NULL = no-op.                          */
static UpdiNvmProgressCb g_nvm_progress_cb   = NULL;
static void             *g_nvm_progress_user = NULL;

/* ── Multi-family per-device memory maps (HLR-046; declared in updi.h).
 *
 *  Address evidence (extracted with `pdftotext -layout` from datasheets
 *  in doc/reference/):
 *
 *    AVR-DA  SIGROW@0x1100  USERROW 0x1080/32 B   EEPROM 0x1400/512 B
 *    AVR-DB  SIGROW@0x1100  USERROW 0x1080/32 B   EEPROM 0x1400/512 B
 *    AVR-DD  SIGROW@0x1100  USERROW 0x1080/128 B  EEPROM 0x1400/256 B
 *    AVR-DU  SIGROW@0x1080  USERROW 0x1200/512 B  EEPROM 0x1400/256 B
 *    AVR-SD  SIGROW@0x1080  USERROW 0x1200/512 B  EEPROM 0x1400/256 B
 *
 *  LOCK (0x1040/4 B) and FUSE (0x1050/16 B) are identical across all.  */
static const UpdiDeviceMap g_device_table[] = {
    /* AVR-DA — bench-validated reference family (HLR-046 E1/E2/E3).    */
    { "AVR-DA",
      0x001080u, 0x000020u,   /* USERROW base / size                   */
      0x001400u, 0x000200u,   /* EEPROM  base / size                   */
      0x001050u, 0x000010u,   /* FUSES   base / size                   */
      0x001040u, 0x000004u,   /* LOCK    base / size                   */
      0x001100u,              /* SIGROW base                           */
      true                    /* hw-tested on AVR128DA28               */
    },
    { "AVR-DB",
      0x001080u, 0x000020u,
      0x001400u, 0x000200u,
      0x001050u, 0x000010u,
      0x001040u, 0x000004u,
      0x001100u,
      false
    },
    { "AVR-DD",
      0x001080u, 0x000080u,   /* 128-byte USERROW per memory map       */
      0x001400u, 0x000100u,   /* 256 B EEPROM                          */
      0x001050u, 0x000010u,
      0x001040u, 0x000004u,
      0x001100u,
      false
    },
    { "AVR-DU",
      0x001200u, 0x000200u,   /* 512 B USERROW @ 0x1200                */
      0x001400u, 0x000100u,
      0x001050u, 0x000010u,
      0x001040u, 0x000004u,
      0x001080u,              /* SIGROW moved to 0x1080 on DU          */
      false
    },
    { "AVR-SD",
      0x001200u, 0x000200u,
      0x001400u, 0x000100u,
      0x001050u, 0x000010u,
      0x001040u, 0x000004u,
      0x001080u,
      false
    },
};

/* Default to the AVR-DA entry so call paths that bypass
 * `updi_select_device()` (notably the unit tests, which link `updi.o`
 * and exercise NVM writers directly via PTY harnesses without calling
 * `updi_open()`) see the historical AVR-DA addresses.                  */
static const UpdiDeviceMap *g_device = &g_device_table[0];

/* Part-name prefix → family map.  Each ELF deviceinfo string begins
 * with "avr" + digits + family-letters (e.g. "avr128da28" → "da", thus
 * AVR-DA).  The match is performed by scanning the part-name past the
 * leading "avr<digits>" run and comparing the next two lowercase
 * letters to each row's `letters` field.                              */
static const struct {
    const char letters[3];   /* e.g. "da", "db", "dd", "du", "sd"     */
    const char *family;      /* table family string                    */
} g_partname_prefix[] = {
    { "da", "AVR-DA" },
    { "db", "AVR-DB" },
    { "dd", "AVR-DD" },
    { "du", "AVR-DU" },
    { "sd", "AVR-SD" },
};

const char *updi_family_from_partname(const char *partname)
{
    if (partname == NULL || partname[0] == '\0')
        return NULL;
    /* Expect lowercase "avr" prefix. */
    if (partname[0] != 'a' || partname[1] != 'v' || partname[2] != 'r')
        return NULL;
    /* Skip one or more ASCII digits (the flash-size field, e.g. 128). */
    size_t i = 3;
    if (partname[i] < '0' || partname[i] > '9')
        return NULL;
    while (partname[i] >= '0' && partname[i] <= '9')
        i++;
    /* Need at least two family-letters after the digits. */
    if (partname[i] == '\0' || partname[i + 1] == '\0')
        return NULL;
    char a = partname[i];
    char b = partname[i + 1];
    /* Normalise to lowercase. */
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    for (size_t j = 0;
         j < sizeof(g_partname_prefix) / sizeof(g_partname_prefix[0]);
         j++) {
        if (g_partname_prefix[j].letters[0] == a
            && g_partname_prefix[j].letters[1] == b) {
            return g_partname_prefix[j].family;
        }
    }
    return NULL;
}

const UpdiDeviceMap *updi_get_device(void)
{
    return g_device;
}

static int case_eq(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Autodetect helper: known AVR-DA / AVR-DB SIGROW.DEVICEID triplets
 * (identical to main.c's `device_family[]`).  Returns "AVR-DA",
 * "AVR-DB", or NULL.  AVR-DD signatures are not listed here because
 * the bench has not exercised them; users select DD/DU/SD explicitly
 * via `--force-device`.                                                */
static const char *autodetect_family(const uint8_t id[3])
{
    if (id[0] != 0x1E) return NULL;
    /* DEVICEID1: 0x95=32K, 0x96=64K, 0x97=128K.  DEVICEID2 low nibble
     * encodes the family-specific pin/variant code; high nibble is the
     * family discriminator (0=DA, 1=DB on flash-128K; ranges differ on
     * smaller-flash parts).  Match the exact table.                   */
    static const struct { uint8_t id[3]; const char *fam; } known[] = {
        /* AVR128DA */
        { { 0x1E, 0x97, 0x0A }, "AVR-DA" }, { { 0x1E, 0x97, 0x09 }, "AVR-DA" },
        { { 0x1E, 0x97, 0x08 }, "AVR-DA" }, { { 0x1E, 0x97, 0x07 }, "AVR-DA" },
        /* AVR64DA  */
        { { 0x1E, 0x96, 0x15 }, "AVR-DA" }, { { 0x1E, 0x96, 0x14 }, "AVR-DA" },
        { { 0x1E, 0x96, 0x13 }, "AVR-DA" }, { { 0x1E, 0x96, 0x12 }, "AVR-DA" },
        /* AVR32DA  */
        { { 0x1E, 0x95, 0x36 }, "AVR-DA" }, { { 0x1E, 0x95, 0x35 }, "AVR-DA" },
        { { 0x1E, 0x95, 0x34 }, "AVR-DA" },
        /* AVR128DB */
        { { 0x1E, 0x97, 0x0E }, "AVR-DB" }, { { 0x1E, 0x97, 0x0D }, "AVR-DB" },
        { { 0x1E, 0x97, 0x0C }, "AVR-DB" }, { { 0x1E, 0x97, 0x0B }, "AVR-DB" },
        /* AVR64DB  */
        { { 0x1E, 0x96, 0x19 }, "AVR-DB" }, { { 0x1E, 0x96, 0x18 }, "AVR-DB" },
        { { 0x1E, 0x96, 0x17 }, "AVR-DB" }, { { 0x1E, 0x96, 0x16 }, "AVR-DB" },
        /* AVR32DB  */
        { { 0x1E, 0x95, 0x3A }, "AVR-DB" }, { { 0x1E, 0x95, 0x39 }, "AVR-DB" },
        { { 0x1E, 0x95, 0x38 }, "AVR-DB" },
    };
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); i++) {
        if (known[i].id[0] == id[0] &&
            known[i].id[1] == id[1] &&
            known[i].id[2] == id[2])
            return known[i].fam;
    }
    return NULL;
}

int updi_select_device(int fd, const char *force_family)
{
    if (force_family != NULL && force_family[0] != '\0') {
        for (size_t i = 0; i < sizeof(g_device_table)/sizeof(g_device_table[0]); i++) {
            if (case_eq(force_family, g_device_table[i].family)) {
                g_device = &g_device_table[i];
                if (!g_device->hw_tested) {
                    fprintf(stderr,
                            "warning: --force-device=%s selects a "
                            "family that has NOT been hardware-validated; "
                            "use at your own risk\n",
                            g_device->family);
                }
                return 0;
            }
        }
        fprintf(stderr,
                "error: --force-device='%s' does not match any supported "
                "family (AVR-DA, AVR-DB, AVR-DD, AVR-DU, AVR-SD)\n",
                force_family);
        return -1;
    }

    /* Autodetect probe path.  Read 3 bytes at the AVR-Dx SIGROW base
     * (0x1100); on AVR-DU / AVR-SD this address holds Flash code and
     * the DEVICEID0 byte will not be 0x1E — the user must pass
     * `--force-device=<family>` for those chips.                      */
    uint8_t sig[3];
    if (updi_mem_read(fd, 0x1100u, sig, 3u) < 0) {
        fprintf(stderr,
                "error: updi_select_device: SIGROW@0x1100 read failed; "
                "use --force-device=<AVR-DA|AVR-DB|AVR-DD|AVR-DU|AVR-SD>\n");
        return -1;
    }
    const char *fam = autodetect_family(sig);
    if (fam == NULL) {
        fprintf(stderr,
                "error: SIGROW signature %02X %02X %02X did not match "
                "any AVR-DA/DB device; pass --force-device=<family> "
                "(AVR-DA, AVR-DB, AVR-DD, AVR-DU, AVR-SD) to override\n",
                sig[0], sig[1], sig[2]);
        return -1;
    }
    for (size_t i = 0; i < sizeof(g_device_table)/sizeof(g_device_table[0]); i++) {
        if (case_eq(fam, g_device_table[i].family)) {
            g_device = &g_device_table[i];
            return 0;
        }
    }
    /* Cannot happen — `autodetect_family` only returns names that
     * exist in the table.                                              */
    return -1;
}

/* ── poll counts (each iteration = 1 ms via nanosleep) ─────────────────── */
#define UPDI_NVMPROG_POLL_MAX    100   /* 100 ms */
#define UPDI_PAGE_BUSY_POLL_MAX   20   /*  20 ms */
#define UPDI_READ_TIMEOUT_US  100000   /* 100 ms select() timeout */

/* ── private forward declarations ──────────────────────────────────────── */

static speed_t baud_to_speed(int baud);
static int     updi_write_bytes(int fd, const uint8_t *buf, size_t n);
static int     updi_ldcs(int fd, uint8_t cs);
static int     updi_stcs(int fd, uint8_t cs, uint8_t val);
static int     updi_set_ptr(int fd, uint32_t addr);
static int     updi_send_repeat(int fd, uint8_t count_m1);

/* ── private helpers ────────────────────────────────────────────────────── */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 300:    return B300;
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return B115200;
    }
}

/*
 * Write n bytes to fd, then read and discard exactly n half-duplex echo
 * bytes.  Returns 0 on success, -1 if the echo does not arrive within the
 * VTIME window.  Max n is 16 bytes (largest single UPDI frame).
 */
static int updi_write_bytes(int fd, const uint8_t *buf, size_t n)
{
    uint8_t echo[16];
    size_t  total;

    if (n > sizeof(echo))
        return -1;
    if (write(fd, buf, n) != (ssize_t)n)
        return -1;

    total = 0;
    while (total < n) {
        ssize_t got = read(fd, echo + total, n - total);
        if (got <= 0)
            return -1;
        total += (size_t)got;
    }
    return 0;
}

/* SYNCH + (0x80|cs); reads the one-byte response.  Returns byte or -1. */
static int updi_ldcs(int fd, uint8_t cs)
{
    uint8_t cmd[2] = { UPDI_SYNCH, (uint8_t)(UPDI_OP_LDCS_PREFIX | cs) };
    uint8_t resp;

    if (updi_write_bytes(fd, cmd, 2) < 0)
        return -1;
    if (read(fd, &resp, 1) != (ssize_t)1)
        return -1;
    return (int)(unsigned int)resp;
}

/* SYNCH + (0xC0|cs) + val.  Returns 0 on success, -1 on error. */
static int updi_stcs(int fd, uint8_t cs, uint8_t val)
{
    uint8_t cmd[3] = { UPDI_SYNCH, (uint8_t)(UPDI_OP_STCS_PREFIX | cs), val };
    return updi_write_bytes(fd, cmd, 3);
}

/*
 * Set the UPDI pointer register using the 24-bit ST_PTR_LONG form:
 *   SYNCH, ST_PTR_LONG (0x6A), addr_lo, addr_mid, addr_hi   → one ACK
 *
 * ALWAYS use the 24-bit form, even for addresses <= 0xFFFF.  The 16-bit
 * ST_PTR_WORD form updates only the low two pointer bytes and leaves the
 * high byte (bits 16..23) at whatever a previous ST_PTR_LONG set it to.
 * AVR-Dx mapped-Flash reads use ST_PTR_LONG with a high byte of 0x80, so a
 * subsequent ST_PTR_WORD read of SRAM (UPDI 0x4000..0x7FFF) would be
 * misdirected to 0x80xxxx — mapped Flash — and return erased 0xFF on a
 * sparsely-programmed part.  Because GDB interleaves Flash reads (code and
 * line tables) with stack reads, that produced intermittently garbage
 * locals and bogus (0x1fffe) backtraces whenever a Flash read preceded a
 * stack read.  Writing the full 24-bit pointer every time removes the
 * stale-high-byte hazard — this is also what avrdude's serialupdi does.
 *
 * Expects one ACK back from the UPDI (datasheet §35.3.3.4).
 */
static int updi_set_ptr(int fd, uint32_t addr)
{
    uint8_t frame[5] = {
        UPDI_SYNCH, UPDI_OP_ST_PTR_LONG,
        (uint8_t)( addr        & 0xFFu),
        (uint8_t)((addr >>  8) & 0xFFu),
        (uint8_t)((addr >> 16) & 0xFFu),
    };
    uint8_t ack;

    if (updi_write_bytes(fd, frame, sizeof(frame)) < 0)
        return -1;
    if (read(fd, &ack, 1) != (ssize_t)1)
        return -1;
    if (ack != UPDI_ACK)
        return -1;
    return 0;
}

/* SYNCH + REPEAT + count.  REPEAT does not produce an ACK. */
static int updi_send_repeat(int fd, uint8_t count_m1)
{
    uint8_t frame[3] = { UPDI_SYNCH, UPDI_OP_REPEAT, count_m1 };
    return updi_write_bytes(fd, frame, sizeof(frame));
}

/*
 * Enter NVMPROG mode (datasheet §35.3.7.2 steps 2-7).
 *
 * Sends the 8-byte "NVMProg " KEY (LSB-first), pulses ASI_RESET_REQ to
 * latch the key, then polls ASI_SYS_STATUS.NVMPROG (bit 3) until set.
 *
 * Holding the target in NVMPROG keeps the CPU halted, which is required
 * any time the host needs reliable repeated memory accesses — otherwise
 * a target running firmware that enters SLEEP (e.g. `_exit` → `cli; sleep`
 * on a bare-metal fixture) will fail every UPDI read after the initial
 * cold-start handshake.  Both avrdude's serialupdi and pymcuprog enter
 * NVMPROG before any user-visible memory access for exactly this reason.
 *
 * Returns 0 on success, -1 on I/O error or timeout.
 */
static int updi_enter_nvmprog(int fd)
{
    static const uint8_t key_cmd[10] = {
        UPDI_SYNCH, UPDI_OP_KEY,
        /* "NVMProg ", LSB-first per datasheet §35.3.3.13 */
        ' ', 'g', 'o', 'r', 'P', 'M', 'V', 'N'
    };
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    int last_status = -1;

    /* Idempotency guard.  Re-issuing the NVMPROG key while the chip is
     * already in NVMPROG resets out of NVMPROG without re-latching the
     * key (observed empirically: SYS_STATUS=0x82 sticks indefinitely).
     * Avrdude only enters NVMPROG once per session for the same reason. */
    {
        int s = updi_ldcs(fd, ASI_SYS_STATUS);
        if (s >= 0 && (s & ASI_SYS_STATUS_NVMPROG))
            return 0;
    }

    if (updi_write_bytes(fd, key_cmd, sizeof(key_cmd)) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;

    for (int i = 0; i < UPDI_NVMPROG_POLL_MAX; i++) {
        int s = updi_ldcs(fd, ASI_SYS_STATUS);
        if (s < 0)
            return -1;
        last_status = s;
        if (s & ASI_SYS_STATUS_NVMPROG)
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr,
            "updi_enter_nvmprog: NVMPROG bit never set (last SYS_STATUS=0x%02x)\n",
            last_status & 0xFF);
    return -1;
}

/*
 * LDS (24-bit address, 8-bit data): direct single-byte load from any
 * 24-bit physical address.  Wire: SYNCH, 0x08, addr_lo, addr_mid, addr_hi.
 * Target responds with one data byte.  Used for NVMCTRL / SIGROW access
 * where avrdude's serialupdi uses LDS rather than ST_PTR+LD_PTR_INC.
 */
static int updi_lds8(int fd, uint32_t addr, uint8_t *out)
{
    uint8_t frame[5] = {
        UPDI_SYNCH, UPDI_OP_LDS_24_8,
        (uint8_t)( addr        & 0xFFu),
        (uint8_t)((addr >>  8) & 0xFFu),
        (uint8_t)((addr >> 16) & 0xFFu),
    };
    if (updi_write_bytes(fd, frame, sizeof(frame)) < 0)
        return -1;
    if (read(fd, out, 1) != (ssize_t)1)
        return -1;
    return 0;
}

/*
 * STS (24-bit address, 8-bit data): direct single-byte store to any
 * 24-bit physical address.  Wire: SYNCH, 0x48, addr_lo, addr_mid, addr_hi;
 * target ACKs; then send data byte; target ACKs.  Used for NVMCTRL writes.
 */
static int updi_sts8(int fd, uint32_t addr, uint8_t val)
{
    uint8_t frame[5] = {
        UPDI_SYNCH, UPDI_OP_STS_24_8,
        (uint8_t)( addr        & 0xFFu),
        (uint8_t)((addr >>  8) & 0xFFu),
        (uint8_t)((addr >> 16) & 0xFFu),
    };
    uint8_t ack;

    if (updi_write_bytes(fd, frame, sizeof(frame)) < 0)
        return -1;
    if (read(fd, &ack, 1) != (ssize_t)1 || ack != UPDI_ACK)
        return -1;
    if (updi_write_bytes(fd, &val, 1) < 0)
        return -1;
    if (read(fd, &ack, 1) != (ssize_t)1 || ack != UPDI_ACK)
        return -1;
    return 0;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int updi_open(const char *device, int baud)
{
    struct termios tty;
    speed_t        speed;
    int            fd;
    int            attempt;

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (fcntl(fd, F_SETFL, 0) < 0) {
        close(fd);
        return -1;
    }

    if (tcgetattr(fd, &tty) < 0) {
        close(fd);
        return -1;
    }

    /* raw mode (cfmakeraw equivalent) */
    tty.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag &= ~(tcflag_t)OPOST;
    tty.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /* 8E2: 8 data bits, even parity, 2 stop bits.
     *   PARENB = 1, PARODD = 0 ⇒ even parity (datasheet §35.3.1).        */
    tty.c_cflag &= ~(tcflag_t)CSIZE;
    tty.c_cflag &= ~(tcflag_t)PARODD;
    tty.c_cflag |= CS8 | CSTOPB | PARENB | CLOCAL | CREAD;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;   /* 100 ms read timeout */

    speed = baud_to_speed(baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    /* On Linux PTYs the kernel rejects PARENB on a reopened slave
     * (parity is unsupported on virtual terminals).  Try 8E2 first —
     * required by the UPDI spec on real serial ports — and fall back to
     * 8N2 only if the kernel refuses the parity bit.                    */
    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        if (errno != EINVAL) {
            close(fd);
            return -1;
        }
        tty.c_cflag &= ~(tcflag_t)PARENB;
        if (tcsetattr(fd, TCSANOW, &tty) < 0) {
            close(fd);
            return -1;
        }
    }

    /*
     * Cold-start handshake (datasheet §35.3.1.2 + §35.3.2.3, matching
     * avrdude's `serialupdi` programmer; see issue #19):
     *
     *   1. Write one 0x00 wake byte.  Its low frame doubles as a soft
     *      BREAK at session baud — ≈87 µs of line-low, well above the
     *      UPDI 12-bit-time minimum at any UPDI-clock rate in use.
     *   2. tcdrain to flush TX, then tcflush(TCIFLUSH) to drop the
     *      half-duplex echo of the wake byte.  Without the flush the
     *      stale echo shifts the echo-byte accounting in every later
     *      `updi_write_bytes()` call by one byte, which makes the
     *      first `updi_set_ptr()` read an echo byte where it expects
     *      the ACK and abort.
     *   3. `STCS ASI_CTRLB = CCDETDIS` so the target ignores the
     *      contention between its open-drain UPDI TX and the host
     *      UART's push-pull idle-high TX.  Passive single-wire
     *      combiners (the typical wiring on Raspberry Pi PL011 / FTDI
     *      hosts) make the target see every TX attempt as a collision
     *      and stay silent forever unless CCDETDIS is set.
     *   4. `STCS ASI_CTRLA = IBDLY` so the target inserts inter-byte
     *      guard time on its responses, which the host UART RX needs
     *      to deframe reliably at session baud.
     *   5. `LDCS ASI_STATUSA` to verify the link is alive.  STATUSA
     *      carries UPDIREV in its upper nibble and is always readable
     *      once UPDI is enabled, even before any reset has occurred.
     *
     * Retries: the entire 5-step sequence is attempted up to 3 times
     * before declaring a link failure and returning -1.
     */
    /*
     * Cold-start handshake (matching avrdude's `serialupdi` programmer
     * exactly; see issue #19).  Empirically verified by `strace`-ing
     * avrdude against the same target as this debugger:
     *
     *   Attempt 0 (fast path, succeeds against an already-prepped target):
     *     1. Write one 0x00 wake byte at session baud, then
     *        tcdrain() + tcflush(TCIFLUSH) to discard the half-duplex
     *        echo of the wake byte.
     *     2. STCS ASI_CTRLB = CCDETDIS  (disable contention detection)
     *     3. STCS ASI_CTRLA = IBDLY     (inter-byte delay on responses)
     *     4. LDCS ASI_STATUSA          (link probe)
     *
     *   Attempts 1, 2 (slow path, required on cold power-on with passive
     *   single-wire combiners — RPi PL011, FTDI):
     *     1a. Switch the kernel baud rate to 300 baud.
     *     1b. Write two 0x00 bytes — each holds the TX line low for
     *         ≈ 33 ms at 300 baud, giving the target's UPDI clock and
     *         contention detector a clean ≥ 60 ms reset window that
     *         dwarfs anything achievable at session baud.
     *     1c. Restore session baud and tcflush(TCIFLUSH).
     *     2-4. STCS CTRLB / STCS CTRLA / LDCS STATUSA as above.
     *
     * The 300-baud trick is unusual but Microchip-blessed — pymcuprog
     * and avrdude both use it because no portable POSIX API generates
     * a multi-ms BREAK on demand.  tcsendbreak() is too short and
     * not honoured by all kernel UART drivers.
     */
    for (attempt = 0; attempt < 3; attempt++) {
        int rc;

        if (attempt == 0) {
            uint8_t wake = 0x00u;

            if (write(fd, &wake, 1) != (ssize_t)1)
                continue;
            tcdrain(fd);
            tcflush(fd, TCIFLUSH);
        } else {
            struct termios slow = tty;
            uint8_t        brk  = 0x00u;
            uint8_t        echo;

            /* B300, 8 data bits, even parity, 1 stop bit (NOT 2) — exactly
             * matching avrdude's serialupdi.  The 1-stop-bit cell makes the
             * byte ≈ 33 ms long, which is the line-low BREAK duration we
             * want.  CSTOPB is restored when we revert to session baud. */
            cfsetispeed(&slow, B300);
            cfsetospeed(&slow, B300);
            slow.c_cflag &= ~(tcflag_t)CSTOPB;
            if (tcsetattr(fd, TCSADRAIN, &slow) < 0)
                continue;
            tcflush(fd, TCIFLUSH);

            /* Two 0x00 bytes at 300 baud = two ~33 ms line-low pulses.
             * `tcdrain()` after each write is critical: it blocks until the
             * UART has actually clocked the byte out of the FIFO.  Without
             * it, the subsequent `tcsetattr(B115200)` either flushes the
             * queued byte or re-clocks it at 115200 baud, in either case
             * destroying the BREAK pulse.  After each write+drain we
             * read-and-discard the half-duplex echo at 300 baud; if it
             * fails to arrive within VTIME we still proceed (the BREAK
             * pulse itself is what the target needs, not the echo).      */
            if (write(fd, &brk, 1) == (ssize_t)1) {
                tcdrain(fd);
                { ssize_t r = read(fd, &echo, 1); (void)r; }
            }
            if (write(fd, &brk, 1) == (ssize_t)1) {
                tcdrain(fd);
                { ssize_t r = read(fd, &echo, 1); (void)r; }
            }

            /* Allow any in-flight echo bytes from the line bus to reach
             * the kernel RX queue before we switch baud — otherwise they
             * arrive AFTER the tcflush below, get clocked at 115200 baud
             * (corrupt framing), and desynchronise every subsequent
             * frame echo.                                                */
            {
                struct timespec ts = { 0, 50 * 1000 * 1000L };  /* 50 ms */
                (void)nanosleep(&ts, NULL);
            }
            tcflush(fd, TCIFLUSH);

            if (tcsetattr(fd, TCSADRAIN, &tty) < 0)
                continue;
            tcflush(fd, TCIFLUSH);
        }

        rc = updi_stcs(fd, ASI_CTRLB, ASI_CTRLB_CCDETDIS);
        if (rc < 0) continue;
        rc = updi_stcs(fd, ASI_CTRLA, ASI_CTRLA_IBDLY);
        if (rc < 0) continue;
        rc = updi_ldcs(fd, ASI_STATUSA);
        if (rc < 0) continue;

        /*
         * Issue a full system-reset pulse to clear any latched mode
         * (NVMPROG, OCD, UROWPROG, etc.) from a prior session.  This
         * matches avrdude's "device in reset status, trying to release
         * it" recovery in -vvvv traces, except that we do it
         * unconditionally — empirically the chip can be left with
         * SYS_STATUS bit 7 set after an aborted debug session and
         * refuses NVMPROG entry until reset is re-pulsed.
         *
         *   STCS 0x59 → ASI_RESET_REQ   ; assert reset
         *   STCS 0x00 → ASI_RESET_REQ   ; release reset
         *
         * Both writes are harmless on a chip that is not already in
         * reset.  See doc/reference/guesswork.md §ASI_RESET_REQ.
         */
        rc = updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET);
        if (rc < 0) continue;
        rc = updi_stcs(fd, ASI_RESET_REQ, 0x00u);
        if (rc < 0) continue;

        /*
         * Wake the target from UPDI SLEEP if necessary.  Reading the
         * 16-byte System Information Block (SYNCH + 0xE6, then 16
         * response bytes) is harmless on an awake target and reliably
         * wakes a sleeping one — avrdude's serialupdi issues exactly
         * this transaction whenever ASI_SYS_STATUS reports INSLEEP.
         * Without this step a target left in SLEEP from a prior session
         * will accept the link-layer probes (LDCS STATUSA) but reject
         * every memory access (sigrow read returns -1).
         */
        {
            uint8_t sib_cmd[2] = { UPDI_SYNCH, UPDI_OP_KEY_SIB };
            uint8_t sib_buf[UPDI_SIB_LEN];
            size_t  total;
            int     ok = 1;

            if (updi_write_bytes(fd, sib_cmd, sizeof(sib_cmd)) < 0)
                continue;
            total = 0;
            while (total < sizeof(sib_buf)) {
                ssize_t got = read(fd, sib_buf + total, sizeof(sib_buf) - total);
                if (got <= 0) { ok = 0; break; }
                total += (size_t)got;
            }
            if (!ok) continue;
        }

        /*
         * Enter NVMPROG to halt the CPU.  This prevents target firmware
         * that immediately enters SLEEP after `main()` returns (or any
         * other power-management code path) from making subsequent UPDI
         * memory reads time out.  We do this unconditionally because
         * even an erased target benefits (its execution of 0xFFFF =
         * undefined opcode is unpredictable) and the chip-erase /
         * NVM-write paths re-issue the NVMPROG key anyway, which is
         * idempotent.
         */
        if (updi_enter_nvmprog(fd) < 0)
            continue;

        return fd;
    }

    close(fd);
    return -1;
}

void updi_close(int fd)
{
    if (fd < 0)
        return;
    /* Avrdude-style clean exit (verified by -vvvv trace):
     *
     *   STCS 0x59 → ASI_RESET_REQ   ; assert system reset
     *   STCS 0x00 → ASI_RESET_REQ   ; release — chip exits NVMPROG/OCD
     *   STCS 0x0C → ASI_CTRLB       ; UPDIDIS + CCDETDIS (link teardown)
     *
     * The reset pulse clears any latched NVMPROG/OCD mode so the next
     * --open does not see SYS_STATUS with stale bits set (notably the
     * 0x82 we observed after a debug session, which blocked NVMPROG
     * entry).
     *
     * IMPORTANT: We must explicitly clear the NVMProg key latch before
     * asserting reset. If the key is still latched, the reset pulse
     * will simply command the CPU to re-enter NVMPROG mode and halt.
     * Errors are intentionally ignored — close() must succeed
     * even if the target stopped responding.                          */
    (void)updi_stcs(fd, ASI_KEY_STATUS, ASI_KEY_STATUS_NVMPROG);
    (void)updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET);
    (void)updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN);
    (void)updi_stcs(fd, ASI_CTRLB,
                    (uint8_t)(ASI_CTRLB_CCDETDIS | 0x04u /* UPDIDIS */));
    tcdrain(fd);
    close(fd);
}

/*
 * mem_read: split into three frames per block (datasheet §35.3.3.3,
 * §35.3.3.4, §35.3.3.7):
 *     A) SYNCH, ST_PTR_{WORD,LONG}, addr bytes (LE) → ACK
 *     B) SYNCH, REPEAT, count-1                     (no ACK)
 *     C) SYNCH, LD ptr++                            (no ACK)
 * The UPDI then streams `count` data bytes after the guard time.
 * `updi_set_ptr()` selects ST_PTR_WORD (16-bit) for addr ≤ 0xFFFF and
 * ST_PTR_LONG (24-bit) above, enabling access to mapped Flash on
 * AVR128DA/DB parts.
 */
int updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        size_t         block_len = len - done;
        uint32_t       cur_addr  = addr + (uint32_t)done;
        fd_set         rfds;
        struct timeval tv;
        size_t         got;
        uint8_t        ld_frame[2] = { UPDI_SYNCH, UPDI_OP_LD_PTR_INC };

        if (block_len > UPDI_MAX_BLOCK)
            block_len = UPDI_MAX_BLOCK;

        if (updi_set_ptr(fd, cur_addr) < 0)
            return -1;
        if (updi_send_repeat(fd, (uint8_t)(block_len - 1u)) < 0)
            return -1;
        if (updi_write_bytes(fd, ld_frame, sizeof(ld_frame)) < 0)
            return -1;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = UPDI_READ_TIMEOUT_US;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            return -1;

        got = 0;
        while (got < block_len) {
            ssize_t r = read(fd, buf + done + got, block_len - got);
            if (r <= 0)
                return -1;
            got += (size_t)r;
        }
        done += block_len;
    }
    return 0;
}

/*
 * mem_write: same three-frame setup as mem_read, then per-byte data with
 * one ACK from the UPDI per byte (datasheet §35.3.3.4 + Fig. 35-13).
 */
int updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        size_t   block_len = len - done;
        uint32_t cur_addr  = addr + (uint32_t)done;
        size_t   i;
        uint8_t  st_frame[2] = { UPDI_SYNCH, UPDI_OP_ST_PTR_INC };

        if (block_len > UPDI_MAX_BLOCK)
            block_len = UPDI_MAX_BLOCK;

        if (updi_set_ptr(fd, cur_addr) < 0)
            return -1;
        if (updi_send_repeat(fd, (uint8_t)(block_len - 1u)) < 0)
            return -1;
        if (updi_write_bytes(fd, st_frame, sizeof(st_frame)) < 0)
            return -1;

        for (i = 0; i < block_len; i++) {
            uint8_t ack;
            if (updi_write_bytes(fd, &buf[done + i], 1) < 0)
                return -1;
            if (read(fd, &ack, 1) != (ssize_t)1)
                return -1;
            if (ack != UPDI_ACK)
                return -1;
        }
        done += block_len;
    }
    return 0;
}

/* ── CPU halt / single-step / run — OCD implementation ────────────────── *
 *                                                                         *
 * The AVR-Dx exposes CPU debug control via an On-Chip Debugger (OCD)      *
 * peripheral that is NOT publicly documented by Microchip.  The register  *
 * map used below is the community-reverse-engineered consensus captured   *
 * in doc/reference/guesswork.md, which has been independently verified    *
 * against open-source debuggers Bloom and pyavrdebug.                     *
 *                                                                         *
 * OCD lives in TWO addressing spaces:                                     *
 *   • ASI CS-space @ 0x04/0x05/0x0D — accessed via LDCS/STCS              *
 *     (link-layer, available without halting the CPU).                    *
 *   • Memory-mapped @ 0x0F80+      — accessed via LDS/STS                 *
 *     (peripheral block, valid only while CPU is halted).                 *
 *                                                                         *
 * Activation requires a separate key handshake ('OCD     ') analogous to  *
 * the NVMPROG key.  After the key is latched and a system-reset pulse is  *
 * issued, the chip enters OCD mode and (because SOR_DIS defaults to 0)    *
 * halts at the reset vector with STOPPED set in ASI_OCD_STATUS.           */

/* Send the 8-byte OCD activation key.  LSB-first per UPDI §35.3.3.13;
 * the bytes on the wire are the reverse of the ASCII string "OCD     ". */
static int updi_send_ocd_key(int fd)
{
    static const uint8_t key_cmd[10] = {
        UPDI_SYNCH, UPDI_OP_KEY,
        /* "OCD     " reversed = "     DCO" */
        ' ', ' ', ' ', ' ', ' ', 'D', 'C', 'O'
    };
    return updi_write_bytes(fd, key_cmd, sizeof(key_cmd));
}

/*
 * Enter OCD (debug) mode.
 *
 *   1. KEY 'OCD     ' (LSB-first)
 *   2. Pulse ASI_RESET_REQ: 0x59 → 0x00
 *   3. Poll ASI_OCD_STATUS.STOPPED until set (CPU halted at reset vector)
 *
 * Once halted, the OCD memory-mapped registers at 0x0F80+ are accessible
 * via standard LDS/STS, and CPU run/halt/step is driven from ASI CS 0x04.
 *
 * Returns 0 on success, -1 on I/O error or timeout.
 */
int updi_enter_debug(int fd)
{
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    int last = -1;

    if (updi_send_ocd_key(fd) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;

    for (int i = 0; i < 200; i++) {
        int s = updi_ldcs(fd, ASI_OCD_STATUS);
        if (s < 0) return -1;
        last = s;
        if (s & ASI_OCD_STATUS_STOPPED)
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr,
            "updi_enter_debug: STOPPED bit never set (last ASI_OCD_STATUS=0x%02x)\n",
            last & 0xFF);
    return -1;
}

int updi_halt(int fd)
{
    struct timespec ts = { 0, 500000L };    /* 0.5 ms */
    if (updi_stcs(fd, ASI_OCD_CTRLA, ASI_OCD_CTRLA_STOP) < 0)
        return -1;
    for (int i = 0; i < 200; i++) {
        int s = updi_ldcs(fd, ASI_OCD_STATUS);
        if (s < 0) return -1;
        if (s & ASI_OCD_STATUS_STOPPED)
            return 0;
        nanosleep(&ts, NULL);
    }
    return -1;
}

int updi_run(int fd)
{
    /* Defensive: clear OCD_CTRL0.STEP before resuming.  updi_step()
     * arms the STEP bit to single-instruction the CPU; the silicon
     * leaves it set after the step completes, so a subsequent plain
     * RUN would behave as another single step.  Clearing here makes
     * `c` (continue) reliably free-run regardless of how we got
     * halted.                                                       */
    uint8_t c0 = 0;
    if (updi_lds8(fd, OCD_CTRL0, &c0) < 0) return -1;
    if (c0 & OCD_CTRL0_STEP) {
        if (updi_sts8(fd, OCD_CTRL0,
                      (uint8_t)(c0 & (uint8_t)~OCD_CTRL0_STEP)) < 0)
            return -1;
    }
    /* Writing RUN starts the CPU.  STOPPED clears immediately. */
    return updi_stcs(fd, ASI_OCD_CTRLA, ASI_OCD_CTRLA_RUN);
}

int updi_step(int fd)
{
    /* Set OCD CTRL0.STEP, then RUN.  The CPU executes one instruction
     * and re-asserts STOPPED.  Per guesswork.md the trick is reliable
     * provided UPDICLKSEL is left at default (32 MHz). */
    uint8_t c0 = 0;
    if (updi_lds8(fd, OCD_CTRL0, &c0) < 0) return -1;
    if (updi_sts8(fd, OCD_CTRL0, (uint8_t)(c0 | OCD_CTRL0_STEP)) < 0) return -1;
    if (updi_stcs(fd, ASI_OCD_CTRLA, ASI_OCD_CTRLA_RUN) < 0) return -1;
    /* Wait for halt. */
    {
        struct timespec ts = { 0, 200000L };  /* 0.2 ms */
        for (int i = 0; i < 500; i++) {
            int s = updi_ldcs(fd, ASI_OCD_STATUS);
            if (s < 0) return -1;
            if (s & ASI_OCD_STATUS_STOPPED) return 0;
            nanosleep(&ts, NULL);
        }
    }
    return -1;
}

int updi_step_32bit(int fd, int bp_slot,
                    uint32_t target_pc, bool halt_on_jump)
{
    /* The `halt_on_jump` parameter is retained for API stability but
     * intentionally NOT applied to OCD_CTRL1_JMP.  Hardware experiment
     * (issue #40, Phase B) showed that arming OCD_CTRL1_JMP together
     * with a HW BP slot misses the *first* change-of-flow after RUN —
     * a step over `CALL fsmDispatch` would silently run through the
     * callee and halt at the next outer-frame CoF instead.  Relying
     * solely on the HW BP at `target_pc` (which RSP computes from the
     * opcode for direct CALL/JMP, or PC+4 for LDS/STS) is reliable for
     * every case the workaround actually needs.                       */
    (void)halt_on_jump;

    bool halted = false;
    int total_wait_ms = 0;
    const int max_wait_ms = 200;

    if (bp_slot < 0 || bp_slot > 1) return -1;

    if (updi_ocd_set_hw_bp(fd, bp_slot, target_pc) < 0) return -1;

    if (updi_run(fd) < 0) goto fail;

    while (total_wait_ms < max_wait_ms) {
        int s = updi_ocd_poll_halted(fd, 10);
        if (s == 0) {
            halted = true;
            break;
        }
        if (s < 0) break;
        total_wait_ms += 10;
    }

    if (!halted) goto fail;

    (void)updi_ocd_clear_hw_bp(fd, bp_slot);
    return 0;

fail:
    (void)updi_halt(fd);
    (void)updi_ocd_clear_hw_bp(fd, bp_slot);
    return -1;
}

int updi_ocd_emulate_cof_32bit(int fd,
                               uint32_t push_return_byte_addr,
                               uint32_t target_byte_addr)
{
    /* If asked, push the AVR-Dx 2-byte return address onto the SRAM
     * stack.  Although guesswork.md notes a "17-bit PC", the
     * AVR128DA28/32/48/64 part has exactly 128 KB = 64 KW = 2**16 word
     * addresses of flash, so PC is 16 bits wide and CALL/RET use a
     * 2-byte stack frame.  Empirically validated on AVR128DA48
     * (issue #40, finish-from-fsmDispatch experiment): RET pops
     * exactly two bytes — SP advances by +2 — and the third byte we
     * had been pushing remained as untouched stack garbage.  AVR ISA
     * convention for CALL on 16-bit-PC parts:
     *     mem[SP    ] = PCL
     *     mem[SP - 1] = PCH
     *     SP <- SP - 2
     * `push_return_byte_addr` is the GDB-style BYTE address of the
     * instruction immediately following the CALL we are emulating.   */
    if (push_return_byte_addr != 0u) {
        uint16_t sp = 0;
        if (updi_ocd_read_sp(fd, &sp) < 0) return -1;
        /* The CPU's real SP lives in I/O-space SPL/SPH (data 0x003D /
         * 0x003E).  OCD_SP at OCD+0x18 is a debug-side mirror only:
         * writing it shows the new value on subsequent `g`-packet
         * reads but the CPU keeps using the unmodified real SP — its
         * very next PUSH would clobber the bytes we just wrote.      */
        uint32_t ret_word = push_return_byte_addr >> 1u;
        uint8_t  pcl = (uint8_t)( ret_word        & 0xFFu);
        uint8_t  pch = (uint8_t)((ret_word >>  8) & 0xFFu);
        if (updi_mem_write(fd, (uint32_t)(sp - 1u), &pch, 1) < 0) return -1;
        if (updi_mem_write(fd, sp,                  &pcl, 1) < 0) return -1;
        uint16_t new_sp = (uint16_t)(sp - 2u);
        uint8_t  spl = (uint8_t)( new_sp       & 0xFFu);
        uint8_t  sph = (uint8_t)((new_sp >> 8) & 0xFFu);
        if (updi_mem_write(fd, 0x003Du, &spl, 1) < 0) return -1;
        if (updi_mem_write(fd, 0x003Eu, &sph, 1) < 0) return -1;
        if (updi_ocd_write_sp(fd, new_sp) < 0) return -1;
        if (updi_ocd_write_pc(fd, target_byte_addr) < 0) return -1;
        if (updi_ocd_stabilize_pc_after_write(fd) < 0) return -1;
        /* Re-write SPL/SPH and OCD_SP AFTER stabilize too — the
         * pipeline-settle NOP step under PCHOLD has been observed to
         * resync the CPU's real SP from the OCD shadow.  Belt and
         * braces (issue #40, Phase C). */
        if (updi_mem_write(fd, 0x003Du, &spl, 1) < 0) return -1;
        if (updi_mem_write(fd, 0x003Eu, &sph, 1) < 0) return -1;
        if (updi_ocd_write_sp(fd, new_sp) < 0) return -1;
        return 0;
    }

    if (updi_ocd_write_pc(fd, target_byte_addr) < 0) return -1;
    return updi_ocd_stabilize_pc_after_write(fd);
}

int updi_ocd_poll_halted(int fd, int timeout_ms)
{
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    int budget = (timeout_ms <= 0) ? 1 : timeout_ms;
    for (int i = 0; i < budget; i++) {
        int s = updi_ldcs(fd, ASI_OCD_STATUS);
        if (s < 0) return -1;                       /* UPDI I/O error */
        if (s & ASI_OCD_STATUS_STOPPED) return 0;   /* halted */
        nanosleep(&ts, NULL);
    }
    return 1;   /* link OK, target still running within budget */
}

int updi_ocd_read_halt_status(int fd, uint8_t *st0, uint8_t *st1)
{
    if (st0 && updi_lds8(fd, OCD_STATUS0, st0) < 0) return -1;
    if (st1 && updi_lds8(fd, OCD_STATUS1, st1) < 0) return -1;
    return 0;
}

int updi_ocd_read_gpr(int fd, uint8_t n, uint8_t *val)
{
    if (n > 31u) return -1;
    return updi_lds8(fd, OCD_REGFILE + n, val);
}

int updi_ocd_write_gpr(int fd, uint8_t n, uint8_t val)
{
    if (n > 31u) return -1;
    return updi_sts8(fd, OCD_REGFILE + n, val);
}

int updi_ocd_read_pc(int fd, uint32_t *byte_addr)
{
    uint8_t lo = 0, hi = 0;
    if (updi_lds8(fd, OCD_PC,     &lo) < 0) return -1;
    if (updi_lds8(fd, OCD_PC + 1, &hi) < 0) return -1;
    /* OCD.PC is a *word* address and reads PC+1 by silicon convention
     * (see doc/reference/guesswork.md §"OCD.PC and PC").  Convert to a
     * GDB byte address: (PC_word - 1) * 2.                              */
    uint32_t pc_word = (uint32_t)lo | ((uint32_t)hi << 8);
    if (pc_word == 0u) pc_word = 1u;       /* paranoia: never underflow */
    *byte_addr = (pc_word - 1u) * 2u;
    return 0;
}

int updi_ocd_write_pc(int fd, uint32_t byte_addr)
{
    /* Inverse of updi_ocd_read_pc(): store (byte_addr/2)+1 as a word
     * address.  Beware: per guesswork, a fresh PC write makes the CPU
     * "skip" exactly one instruction on the next step.  Callers that
     * need precise positioning should use instruction injection.        */
    uint32_t pc_word = (byte_addr >> 1u) + 1u;
    if (updi_sts8(fd, OCD_PC,     (uint8_t)( pc_word        & 0xFFu)) < 0) return -1;
    if (updi_sts8(fd, OCD_PC + 1, (uint8_t)((pc_word >> 8u) & 0xFFu)) < 0) return -1;
    return 0;
}

int updi_ocd_stabilize_pc_after_write(int fd)
{
    uint8_t ctrl0 = 0;

    if (updi_lds8(fd, OCD_CTRL0, &ctrl0) < 0) return -1;
    if (updi_sts8(fd, OCD_CTRL0, (uint8_t)(ctrl0 | OCD_CTRL0_PCHOLD)) < 0)
        return -1;
    if (updi_sts8(fd, OCD_INSN0, 0x00u) < 0) goto fail;
    if (updi_sts8(fd, OCD_INSN0 + 1u, 0x00u) < 0) goto fail;
    if (updi_step(fd) < 0) goto fail;
    if (updi_sts8(fd, OCD_CTRL0, ctrl0) < 0) return -1;
    return 0;

fail:
    (void)updi_sts8(fd, OCD_CTRL0, ctrl0);
    return -1;
}

int updi_ocd_read_sp(int fd, uint16_t *val)
{
    uint8_t lo = 0, hi = 0;
    if (updi_lds8(fd, OCD_SP,     &lo) < 0) return -1;
    if (updi_lds8(fd, OCD_SP + 1, &hi) < 0) return -1;
    *val = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    return 0;
}

int updi_ocd_write_sp(int fd, uint16_t val)
{
    if (updi_sts8(fd, OCD_SP,     (uint8_t)( val        & 0xFFu)) < 0) return -1;
    if (updi_sts8(fd, OCD_SP + 1, (uint8_t)((val >> 8u) & 0xFFu)) < 0) return -1;
    return 0;
}

int updi_ocd_read_sreg (int fd, uint8_t *val) { return updi_lds8(fd, OCD_SREG, val); }
int updi_ocd_write_sreg(int fd, uint8_t val)  { return updi_sts8(fd, OCD_SREG, val); }

int updi_ocd_set_hw_bp(int fd, int idx, uint32_t byte_addr)
{
    uint32_t base;
    uint8_t  enable_bit;
    if (idx == 0)      { base = OCD_BP0A; enable_bit = OCD_CTRL1_BP0; }
    else if (idx == 1) { base = OCD_BP1A; enable_bit = OCD_CTRL1_BP1; }
    else return -1;

    /* BPxA is a 17-bit byte-address field with bit 0 always 0
     * (instruction-aligned).  Write 3 bytes; high byte holds bit 16. */
    if (updi_sts8(fd, base,     (uint8_t)( byte_addr        & 0xFEu)) < 0) return -1;
    if (updi_sts8(fd, base + 1, (uint8_t)((byte_addr >>  8) & 0xFFu)) < 0) return -1;
    if (updi_sts8(fd, base + 2, (uint8_t)((byte_addr >> 16) & 0x01u)) < 0) return -1;

    /* Enable the specific BP plus the global HWBP gate. */
    {
        uint8_t c0 = 0, c1 = 0;
        if (updi_lds8(fd, OCD_CTRL0, &c0) < 0) return -1;
        if (updi_lds8(fd, OCD_CTRL1, &c1) < 0) return -1;
        if (updi_sts8(fd, OCD_CTRL1, (uint8_t)(c1 | enable_bit)) < 0) return -1;
        if (updi_sts8(fd, OCD_CTRL0, (uint8_t)(c0 | OCD_CTRL0_HWBP)) < 0) return -1;
    }
    return 0;
}

int updi_ocd_clear_hw_bp(int fd, int idx)
{
    uint8_t enable_bit;
    if (idx == 0)      enable_bit = OCD_CTRL1_BP0;
    else if (idx == 1) enable_bit = OCD_CTRL1_BP1;
    else return -1;

    uint8_t c1 = 0;
    if (updi_lds8(fd, OCD_CTRL1, &c1) < 0) return -1;
    c1 = (uint8_t)(c1 & (uint8_t)~enable_bit);
    if (updi_sts8(fd, OCD_CTRL1, c1) < 0) return -1;

    /* If both BPs disabled, drop the global gate too. */
    if ((c1 & (OCD_CTRL1_BP0 | OCD_CTRL1_BP1)) == 0u) {
        uint8_t c0 = 0;
        if (updi_lds8(fd, OCD_CTRL0, &c0) < 0) return -1;
        c0 = (uint8_t)(c0 & (uint8_t)~OCD_CTRL0_HWBP);
        if (updi_sts8(fd, OCD_CTRL0, c0) < 0) return -1;
    }
    return 0;
}

/* NOTE: AVR-Dx OCD over UPDI does not expose data-address watchpoint
 * hardware.  The previous updi_ocd_{set,clear}_data_bp() entry points
 * wrote to fabricated registers (OCD+0x07/0x0A/0x0E) that the
 * exhaustive FF-bomb in doc/reference/guesswork.md proved are not
 * backed by silicon.  GDB Z2/Z3/Z4 packets now reply with the empty
 * packet and avr-gdb transparently uses software watchpoints.       */

/* Forward decl: defined later in this file, used by updi_nvm_write_flash. */
static int nvm_erase_page(int fd, uint32_t page_addr);

/* Poll NVMCTRL.STATUS until FBUSY clears or the budget is exhausted.
 * AVR-Dx NVMSTATUS exposes only FBUSY (bit 0) and EEBUSY (bit 1); there
 * is no write-error flag, so completion is detected purely by !FBUSY.
 * Returns 0 on success, -1 on timeout / I/O error.                       */
static int nvm_wait_not_busy(int fd, uint32_t page_addr, const char *phase)
{
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    uint8_t last = 0xAAu;
    for (int i = 0; i < 200; i++) {         /* up to 200 ms */
        uint8_t nvm_st = 0;
        if (updi_lds8(fd, NVMCTRL_STATUS, &nvm_st) < 0) {
            fprintf(stderr,
                    "nvm_write_flash: %s NVMSTATUS read failed @0x%06x\n",
                    phase, (unsigned)page_addr);
            return -1;
        }
        last = nvm_st;
        if (!(nvm_st & NVMCTRL_STATUS_FBUSY))
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr,
            "nvm_write_flash: %s FBUSY did not clear @0x%06x (last NVMSTATUS=0x%02x)\n",
            phase, (unsigned)page_addr, last & 0xFF);
    return -1;
}

/*
 * Program one 512-byte FLASH page using the avrdude/pymcuprog sequence:
 *
 *   1. arm NVMCTRL.CTRLA = FLWR via direct STS
 *   2. ST_PTR_LONG ← page_addr (24-bit)
 *   3. STCS ASI_CTRLA = RSD_ON  (target stops ACKing individual stores)
 *   4. REPEAT 0xFF             (256 word-stores follow)
 *   5. ST *(ptr++), word        opcode (0x65) + 512 data bytes
 *   6. STCS ASI_CTRLA = RSD_OFF (restore normal ACK protocol)
 *
 * Each word-store latches into the FLASH page buffer; the AVR-Dx NVM
 * controller commits the buffer to FLASH automatically because FLWR was
 * pre-armed in step 1.  Caller is responsible for waiting FBUSY=0 after.
 */
static int updi_write_page_bulk(int fd, uint32_t page_addr,
                                const uint8_t *data)
{
    uint8_t st_word_frame[2] = { UPDI_SYNCH, UPDI_OP_ST_PTR_INC_W };
    uint8_t echo[64];
    size_t  total;

    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_FLWR) < 0) {
        fprintf(stderr, "updi_write_page_bulk: arm FLWR failed\n");
        return -1;
    }
    if (updi_set_ptr(fd, page_addr) < 0) {
        fprintf(stderr, "updi_write_page_bulk: set_ptr 0x%06x failed\n",
                (unsigned)page_addr);
        return -1;
    }
    if (updi_stcs(fd, ASI_CTRLA, UPDI_CTRLA_RSD_ON) < 0) {
        fprintf(stderr, "updi_write_page_bulk: STCS RSD_ON failed\n");
        return -1;
    }
    if (updi_send_repeat(fd, 0xFFu) < 0) {
        fprintf(stderr, "updi_write_page_bulk: REPEAT failed\n");
        goto restore;
    }
    if (updi_write_bytes(fd, st_word_frame, sizeof(st_word_frame)) < 0) {
        fprintf(stderr, "updi_write_page_bulk: ST_PTR_INC_W failed\n");
        goto restore;
    }

    /* Stream 512 data bytes; no ACK per byte (RSD enabled). */
    if (write(fd, data, UPDI_FLASH_PAGE_SIZE) !=
        (ssize_t)UPDI_FLASH_PAGE_SIZE) {
        fprintf(stderr, "updi_write_page_bulk: page write short\n");
        goto restore;
    }
    total = 0;
    while (total < UPDI_FLASH_PAGE_SIZE) {
        size_t want = UPDI_FLASH_PAGE_SIZE - total;
        if (want > sizeof(echo)) want = sizeof(echo);
        ssize_t got = read(fd, echo, want);
        if (got <= 0) {
            fprintf(stderr,
                    "updi_write_page_bulk: echo read failed after %zu B\n",
                    total);
            goto restore;
        }
        total += (size_t)got;
    }

    if (updi_stcs(fd, ASI_CTRLA, UPDI_CTRLA_RSD_OFF) < 0) {
        fprintf(stderr, "updi_write_page_bulk: STCS RSD_OFF failed\n");
        return -1;
    }
    return 0;

restore:
    /* Best-effort restore — return -1 regardless. */
    (void)updi_stcs(fd, ASI_CTRLA, UPDI_CTRLA_RSD_OFF);
    return -1;
}

int updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data,
                         size_t len)
{
    size_t n_pages;
    size_t pg;

    if ((word_addr % UPDI_FLASH_PAGE_SIZE) != 0u ||
        len == 0u || (len % UPDI_FLASH_PAGE_SIZE) != 0u) {
        fprintf(stderr, "nvm_write_flash: bad align (addr=0x%06x len=%zu)\n",
                (unsigned)word_addr, len);
        return -1;
    }

    /* 1-3. Re-enter NVMPROG (idempotent: updi_open already did it).
     *      Re-issuing the key + reset pulse ensures we are in a clean
     *      programming state even if the caller did `--erase` first,
     *      which leaves the chip in CHIPERASE state.                  */
    if (updi_enter_nvmprog(fd) < 0) {
        fprintf(stderr, "nvm_write_flash: failed to enter NVMPROG\n");
        return -1;
    }

    /* 4. Program each page (matches avrdude/pymcuprog AVR-Dx sequence).
     *    AVR-Dx FLWR only ANDs bits into the existing FLASH cells
     *    (1->0 only).  Without a preceding erase, any cell that is
     *    currently 0 stays 0, silently corrupting the page.  We
     *    therefore issue FLPER (page erase) before every FLWR so the
     *    operation is correct whether or not the caller did --erase.
     *    Re-erasing an already-erased page is harmless (~3 ms).       */
    n_pages = len / UPDI_FLASH_PAGE_SIZE;
    for (pg = 0; pg < n_pages; pg++) {
        uint32_t       page_addr = word_addr +
                                   (uint32_t)(pg * UPDI_FLASH_PAGE_SIZE);
        const uint8_t *page_buf  = data + pg * UPDI_FLASH_PAGE_SIZE;

        if (nvm_wait_not_busy(fd, page_addr, "pre-erase") < 0)
            return -1;
        if (nvm_erase_page(fd, page_addr) < 0) {
            fprintf(stderr,
                    "nvm_write_flash: page 0x%06x erase failed\n",
                    (unsigned)page_addr);
            return -1;
        }
        if (nvm_wait_not_busy(fd, page_addr, "pre-write") < 0)
            return -1;
        if (updi_write_page_bulk(fd, page_addr, page_buf) < 0) {
            fprintf(stderr,
                    "nvm_write_flash: page 0x%06x write failed\n",
                    (unsigned)page_addr);
            return -1;
        }
        if (nvm_wait_not_busy(fd, page_addr, "post-write") < 0)
            return -1;

        /* Optional progress hook: cumulative bytes programmed within
         * this updi_nvm_write_flash() call.  No-op when no callback
         * is installed.                                             */
        if (g_nvm_progress_cb != NULL) {
            size_t done_bytes = (pg + 1u) * UPDI_FLASH_PAGE_SIZE;
            g_nvm_progress_cb(page_addr, done_bytes, len,
                              g_nvm_progress_user);
        }
    }

    /* 5. Clear NVMCTRL.CTRLA = NOCMD.  Leaving FLWR armed has been
     *    observed to corrupt the first ~256 bytes returned by the very
     *    next FLASH read burst on AVR-Dx silicon (the NVM controller's
     *    page buffer remains "open" until the command is cleared or the
     *    bus is exercised enough to flush it).  avrdude/pymcuprog both
     *    issue NOCMD here for the same reason.                        */
    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_NOCMD) < 0) {
        fprintf(stderr, "nvm_write_flash: clear CMD failed\n");
        return -1;
    }

    /* 6. Stay in NVMPROG — the GDB server (or --device) needs the CPU
     *    held halted for subsequent memory reads.  The caller invokes
     *    `updi_close()` to release the chip on shutdown.              */
    return 0;
}

/*
 * Erase a single 512-byte FLASH page (AVR-Dx NVMCTRL FLPER command).
 * AVR-Dx datasheet §6 — FLPER is armed by writing 0x08 to NVMCTRL.CTRLA,
 * then erase is triggered by any FLASH write within the target page.
 * We use a single-byte STS of 0xFF as the trigger.
 * `page_addr` must be the UPDI 24-bit FLASH-space address of the page
 * (i.e. already includes UPDI_FLASH_BASE) and must be page-aligned.
 */
static int nvm_erase_page(int fd, uint32_t page_addr)
{
    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_FLPER) < 0) {
        fprintf(stderr, "nvm_erase_page: arm FLPER failed @0x%06x\n",
                (unsigned)page_addr);
        return -1;
    }
    /* Trigger the erase by writing one byte into the target page. */
    if (updi_sts8(fd, page_addr, 0xFFu) < 0) {
        fprintf(stderr, "nvm_erase_page: trigger STS failed @0x%06x\n",
                (unsigned)page_addr);
        return -1;
    }
    if (nvm_wait_not_busy(fd, page_addr, "erase") < 0)
        return -1;
    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_NOCMD) < 0) {
        fprintf(stderr, "nvm_erase_page: clear CMD failed @0x%06x\n",
                (unsigned)page_addr);
        return -1;
    }
    return 0;
}

/*
 * updi_nvm_flash_patch: read-modify-write FLASH at byte granularity.
 *
 * For each FLASH page touched by [addr, addr+len), the current page is
 * read into a 512-byte buffer, the affected bytes are overwritten, the
 * page is erased (FLPER), and the patched buffer is written back via
 * the bulk page programmer.  Used by the GDB RSP Z0/z0 handlers to
 * plant and remove the AVR BREAK opcode (0x9598) for software
 * breakpoints, where the write is typically 2 bytes at an arbitrary
 * instruction address.
 *
 * `addr` must be a UPDI FLASH-space byte address (i.e. include
 * UPDI_FLASH_BASE).  Length may be any non-zero value; the function
 * spans page boundaries as needed.  The chip remains in NVMPROG on
 * return.
 */
int updi_nvm_flash_patch(int fd, uint32_t addr, const uint8_t *data,
                         size_t len)
{
    const uint32_t page_mask = UPDI_FLASH_PAGE_SIZE - 1u;
    uint8_t  pgbuf[UPDI_FLASH_PAGE_SIZE];
    uint32_t end;
    uint32_t pa;

    if (len == 0u) return 0;

    if (updi_enter_nvmprog(fd) < 0) {
        fprintf(stderr, "nvm_flash_patch: failed to enter NVMPROG\n");
        return -1;
    }

    end = addr + (uint32_t)len;
    for (pa = addr & ~page_mask; pa < end; pa += UPDI_FLASH_PAGE_SIZE) {
        uint32_t pg_end = pa + UPDI_FLASH_PAGE_SIZE;
        uint32_t lo     = (addr > pa)     ? addr : pa;
        uint32_t hi     = (end  < pg_end) ? end  : pg_end;
        size_t   i;

        if (updi_mem_read(fd, pa, pgbuf, UPDI_FLASH_PAGE_SIZE) < 0) {
            fprintf(stderr,
                    "nvm_flash_patch: read page 0x%06x failed\n",
                    (unsigned)pa);
            return -1;
        }
        for (i = 0; i < (hi - lo); i++) {
            pgbuf[(lo - pa) + i] = data[(lo - addr) + i];
        }

        if (nvm_wait_not_busy(fd, pa, "patch-pre") < 0) return -1;
        if (nvm_erase_page(fd, pa) < 0) return -1;
        if (updi_write_page_bulk(fd, pa, pgbuf) < 0) {
            fprintf(stderr,
                    "nvm_flash_patch: write page 0x%06x failed\n",
                    (unsigned)pa);
            return -1;
        }
        if (nvm_wait_not_busy(fd, pa, "patch-post") < 0) return -1;
        if (g_nvm_progress_cb != NULL) {
            size_t done_bytes = (pa + UPDI_FLASH_PAGE_SIZE) - (addr & ~page_mask);
            if (done_bytes > len) done_bytes = len;
            g_nvm_progress_cb(pa, done_bytes, len, g_nvm_progress_user);
        }
    }
    return 0;
}

/*
 * updi_chip_erase: send the CHIPERASE KEY ("NVMErase"), assert system
 * reset, then poll ASI_SYS_STATUS until LOCKSTATUS (bit 1) clears.
 *
 * AVR-Dx datasheet §35.3.7.1 — required to unlock a locked device before
 * NVMPROG can be entered.  DESTRUCTIVE: erases all FLASH and clears the
 * lock fuse.
 */
int updi_chip_erase(int fd)
{
    static const uint8_t key_cmd[10] = {
        UPDI_SYNCH, UPDI_OP_KEY,
        /* 8-byte CHIPERASE key "NVMErase", transmitted LSB-first per
         * datasheet §35.3.3.13 (UPDI KEY opcode). */
        'e', 's', 'a', 'r', 'E', 'M', 'V', 'N'
    };
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    int ks;

    /* 1. Send CHIPERASE key */
    if (updi_write_bytes(fd, key_cmd, sizeof(key_cmd)) < 0) {
        fprintf(stderr, "chip_erase: key write failed\n");
        return -1;
    }

    /* 2. Verify CHIPERASE bit latched in ASI_KEY_STATUS (bit 3) */
    ks = updi_ldcs(fd, ASI_KEY_STATUS);
    if (ks < 0) {
        fprintf(stderr, "chip_erase: ldcs KEY_STATUS failed\n");
        return -1;
    }
    if (!(ks & 0x08u)) {
        fprintf(stderr,
                "chip_erase: CHIPERASE key not latched (KEY_STATUS=0x%02x)\n",
                ks & 0xFF);
        return -1;
    }

    /* 3. Assert system reset, then clear it */
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0) {
        fprintf(stderr, "chip_erase: reset assert failed\n");
        return -1;
    }
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0) {
        fprintf(stderr, "chip_erase: reset clear failed\n");
        return -1;
    }

    /* 4. Poll ASI_SYS_STATUS until LOCKSTATUS (bit 1) clears.
     *    Worst-case chip-erase on AVR128DA is well under 1 s.            */
    for (int i = 0; i < 2000; i++) {
        int s = updi_ldcs(fd, ASI_SYS_STATUS);
        if (s < 0) {
            fprintf(stderr, "chip_erase: ldcs SYS_STATUS failed\n");
            return -1;
        }
        if (!(s & 0x02u))
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "chip_erase: LOCKSTATUS did not clear within 2 s\n");
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────
 * Phase 8 — non-FLASH NVM programming (LLR-UPDI-16..19, HLR-046/047)
 *
 * EEPROM, USERROW (USER_SIGNATURES), FUSES, and LOCK bytes are all
 * programmed via NVMCTRL.CMD = EEERWR (0x13).  Unlike FLASH which
 * commits a 512-byte page buffer, EEERWR performs a byte-wise
 * erase-then-write on every STS into the target window.
 *
 * Sequence (verified against avrdude 7.1 serialupdi wire trace):
 *   1. updi_enter_nvmprog()       (idempotent)
 *   2. wait NVMSTATUS.EEBUSY=0    (precondition)
 *   3. STS(NVMCTRL.CTRLA)=EEERWR  (arm the command ONCE)
 *   4. for each byte:
 *        STS(addr++)=data         (erase+write triggered by the STS)
 *        wait NVMSTATUS.EEBUSY=0  (~4 ms typ., 20 ms max)
 *   5. STS(NVMCTRL.CTRLA)=NOCMD   (disarm)
 *
 * NOTE: Re-arming EEERWR before every byte (CTRLA write between data
 * STSes) caused all but the first byte to be silently dropped on
 * AVR128DA28 — apparently NVMCTRL treats a same-CMD CTRLA write as a
 * no-op only when EEBUSY is clear AND no STS-driven operation is
 * mid-flight; the safe protocol is "arm once, write many, disarm".
 *
 * The chip remains in NVMPROG on return so the caller can issue more
 * writes or transition to OCD via updi_enter_debug().
 * ──────────────────────────────────────────────────────────────────── */

/* Poll NVMCTRL.STATUS until EEBUSY clears or the budget is exhausted.
 * Matches nvm_wait_not_busy() but masks EEBUSY (bit 1) — the EEERWR
 * command does not assert FBUSY.                                       */
static int nvm_wait_not_eebusy(int fd, uint32_t addr, const char *phase)
{
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    uint8_t last = 0xAAu;
    for (int i = 0; i < 200; i++) {         /* up to 200 ms */
        uint8_t nvm_st = 0;
        if (updi_lds8(fd, NVMCTRL_STATUS, &nvm_st) < 0) {
            fprintf(stderr,
                    "nvm_eeprom_write: %s NVMSTATUS read failed @0x%06x\n",
                    phase, (unsigned)addr);
            return -1;
        }
        last = nvm_st;
        if (!(nvm_st & NVMCTRL_STATUS_EEBUSY))
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr,
            "nvm_eeprom_write: %s EEBUSY did not clear @0x%06x (last NVMSTATUS=0x%02x)\n",
            phase, (unsigned)addr, last & 0xFF);
    return -1;
}

/* Common byte-wise EEERWR programmer.  `kind` is a short tag used in
 * diagnostic messages (e.g. "eeprom", "fuses").                        */
static int nvm_eeprom_write_bytes(int fd, uint32_t addr,
                                  const uint8_t *data, size_t len,
                                  const char *kind)
{
    if (len == 0u || data == NULL) {
        fprintf(stderr, "nvm_%s_write: bad args (len=%zu)\n", kind, len);
        return -1;
    }

    if (updi_enter_nvmprog(fd) < 0) {
        fprintf(stderr, "nvm_%s_write: failed to enter NVMPROG\n", kind);
        return -1;
    }

    /* Precondition: NVMCTRL must be idle before arming a new command. */
    if (nvm_wait_not_eebusy(fd, addr, kind) < 0)
        return -1;

    /* Arm EEERWR ONCE for the whole burst.  Re-arming between bytes
     * (writing CTRLA while a CMD is loaded) was observed to silently
     * abort all but the first byte on AVR128DA28. */
    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_EEERWR) < 0) {
        fprintf(stderr,
                "nvm_%s_write: arm EEERWR failed @0x%06x\n",
                kind, (unsigned)addr);
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        if (updi_sts8(fd, addr + (uint32_t)i, data[i]) < 0) {
            fprintf(stderr,
                    "nvm_%s_write: STS @0x%06x failed\n",
                    kind, (unsigned)(addr + i));
            return -1;
        }
        if (nvm_wait_not_eebusy(fd, addr + (uint32_t)i, kind) < 0)
            return -1;
    }

    /* Clear pending command. */
    if (updi_sts8(fd, NVMCTRL_CTRLA, NVMCTRL_CMD_NOCMD) < 0) {
        fprintf(stderr, "nvm_%s_write: clear CMD failed\n", kind);
        return -1;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * USER_SIGNATURES (USERROW) write — special UPDI UROW-key protocol.
 *
 * On AVR-Dx silicon the USER_ROW is NOT programmed through NVMCTRL.CMD
 * like EEPROM/FUSES.  Instead the UPDI peripheral provides a dedicated
 * "UserRowWrite" entry: latch the 64-bit key "NVMUs&te" (LSB-first on
 * the wire), pulse RESET to enter UROWWRITE mode (ASI_KEY_STATUS bit 5),
 * stream the entire 32-byte row via ST_PTR + REPEAT + ST_PTR_INC, then
 * commit by writing ASI_SYS_CTRLA = UROWDONE.  The CPU stays halted
 * throughout and the row programs atomically.
 *
 * Verified against avrdude 7.1 serialupdi -vvvv wire capture on a real
 * AVR128DA28; identical opcode and ASI register sequence as pymcuprog's
 * `write_user_row_locked_device()` path.
 *
 * NOTE: the user row writes atomically as a whole 32-byte page, so a
 * partial-row request must be padded up to 32 bytes with 0xFF.  The
 * caller's window check has already validated [addr, addr+len).
 * ──────────────────────────────────────────────────────────────────── */
/* USER_ROW page size: 32 B on AVR-DA/DB, 128 B on AVR-DD, 512 B on
 * AVR-DU/SD.  The active row length comes from `updi_get_device()`;
 * `UPDI_USERROW_ROW_LEN_MAX` (declared in updi.h) sizes the stack
 * buffer below so a single code path serves every family.            */

static int updi_enter_userrow_write(int fd)
{
    /* "NVMUs&te" stored LSB-first as required by §35.3.3.13. */
    static const uint8_t key_cmd[] = {
        UPDI_SYNCH, UPDI_OP_KEY,
        0x65u, 0x74u, 0x26u, 0x73u, 0x55u, 0x4Du, 0x56u, 0x4Eu,
    };
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    int ks;

    /* UROW key requires the link to already be in NVMPROG so the CPU is
     * halted and the bus is ours.  enter_nvmprog is idempotent. */
    if (updi_enter_nvmprog(fd) < 0)
        return -1;

    if (updi_write_bytes(fd, key_cmd, sizeof(key_cmd)) < 0)
        return -1;

    /* Latch the key by pulsing RESET. */
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;

    /* Confirm UROWWRITE bit set in ASI_KEY_STATUS. */
    for (int i = 0; i < 50; i++) {
        ks = updi_ldcs(fd, ASI_KEY_STATUS);
        if (ks < 0)
            return -1;
        if (ks & ASI_KEY_STATUS_UROWWRITE)
            return 0;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr,
            "updi_enter_userrow_write: UROWWRITE never latched "
            "(last ASI_KEY_STATUS=0x%02x)\n", ks & 0xFF);
    return -1;
}

static int nvm_userrow_write(int fd, uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t row[UPDI_USERROW_ROW_LEN_MAX];
    size_t  off;
    int     st;
    struct timespec ts = { 0, 1000000L };   /* 1 ms */
    const UpdiDeviceMap *dev = updi_get_device();
    const size_t row_len = (size_t)dev->userrow_size;

    if (row_len == 0u || row_len > sizeof(row)) {
        fprintf(stderr,
                "nvm_userrow_write: invalid USERROW row length %zu for %s\n",
                row_len, dev->family);
        return -1;
    }

    /* Pad the caller's partial-row data into a full row-sized image at
     * the correct in-row offset.  Unwritten bytes stay 0xFF.          */
    memset(row, 0xFFu, row_len);
    off = (size_t)(addr - dev->userrow_base);
    if (off + len > row_len) {
        fprintf(stderr,
                "nvm_userrow_write: range [%zu,%zu) overflows %zu-byte row\n",
                off, off + len, row_len);
        return -1;
    }
    memcpy(row + off, data, len);

    if (updi_enter_userrow_write(fd) < 0) {
        fprintf(stderr, "nvm_userrow_write: failed to enter UROW write\n");
        return -1;
    }

    /* Stream the whole row starting at USERROW base. */
    if (updi_mem_write(fd, dev->userrow_base, row, row_len) < 0) {
        fprintf(stderr, "nvm_userrow_write: ST stream failed\n");
        return -1;
    }

    /* Commit per datasheet §35.3.7.3 step 8: write UROWDONE.  We also
     * preserve CLKREQ=1 (its reset-default) so the system clock keeps
     * running while silicon transfers the RAM buffer into the row. */
    if (updi_stcs(fd, ASI_SYS_CTRLA,
                  (uint8_t)(ASI_SYS_CTRLA_UROWDONE | ASI_SYS_CTRLA_CLKREQ)) < 0) {
        fprintf(stderr, "nvm_userrow_write: UROWDONE commit failed\n");
        return -1;
    }

    /* Wait for ASI_SYS_STATUS.UROWPROG to clear (silicon programs the
     * row asynchronously after the commit; typical < 10 ms). */
    for (int i = 0; i < 200; i++) {
        st = updi_ldcs(fd, ASI_SYS_STATUS);
        if (st < 0)
            return -1;
        if (!(st & ASI_SYS_STATUS_UROWPROG))
            break;
        nanosleep(&ts, NULL);
    }
    if (st & ASI_SYS_STATUS_UROWPROG) {
        fprintf(stderr,
                "nvm_userrow_write: UROWPROG did not clear "
                "(last ASI_SYS_STATUS=0x%02x)\n", st & 0xFF);
        return -1;
    }

    /* Datasheet §35.3.7.3 step 11: write UROWWRITE in ASI_KEY_STATUS to
     * reset the programming session.  Writing 1 to a latched-key bit
     * clears it (§35.5.5).  The subsequent reset pulse drops the target
     * out of NVMPROG too, so we re-enter NVMPROG afterwards for any
     * read-back verify the caller may perform. */
    if (updi_stcs(fd, ASI_KEY_STATUS, ASI_KEY_STATUS_UROWWRITE) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RESET) < 0)
        return -1;
    if (updi_stcs(fd, ASI_RESET_REQ, ASI_RESET_REQ_RUN) < 0)
        return -1;
    if (updi_enter_nvmprog(fd) < 0) {
        fprintf(stderr, "nvm_userrow_write: re-enter NVMPROG failed\n");
        return -1;
    }

    return 0;
}

/* Window-guard helper.  Returns 0 if [addr, addr+len) is fully inside
 * [base, base+size); -1 otherwise (with diagnostic).                  */
static int nvm_check_window(const char *kind, uint32_t addr, size_t len,
                            uint32_t base, uint32_t size)
{
    if (len == 0u) {
        fprintf(stderr, "nvm_%s_write: zero-length write rejected\n", kind);
        return -1;
    }
    if (addr < base || (addr - base) + len > size) {
        fprintf(stderr,
                "nvm_%s_write: addr=0x%06x len=%zu outside window "
                "[0x%06x,0x%06x)\n",
                kind, (unsigned)addr, len,
                (unsigned)base, (unsigned)(base + size));
        return -1;
    }
    return 0;
}

int updi_nvm_write_eeprom(int fd, uint32_t addr, const uint8_t *data, size_t len)
{
    const UpdiDeviceMap *dev = updi_get_device();
    if (nvm_check_window("eeprom", addr, len,
                         dev->eeprom_base, dev->eeprom_size) < 0)
        return -1;
    return nvm_eeprom_write_bytes(fd, addr, data, len, "eeprom");
}

int updi_nvm_write_userrow(int fd, uint32_t addr, const uint8_t *data, size_t len)
{
    const UpdiDeviceMap *dev = updi_get_device();
    if (nvm_check_window("userrow", addr, len,
                         dev->userrow_base, dev->userrow_size) < 0)
        return -1;
    return nvm_userrow_write(fd, addr, data, len);
}

int updi_nvm_write_fuses(int fd, uint32_t addr, const uint8_t *data, size_t len)
{
    const UpdiDeviceMap *dev = updi_get_device();
    if (nvm_check_window("fuses", addr, len,
                         dev->fuses_base, dev->fuses_size) < 0)
        return -1;
    return nvm_eeprom_write_bytes(fd, addr, data, len, "fuses");
}

int updi_nvm_write_lockbits(int fd, uint32_t addr, const uint8_t *data,
                            size_t len, bool allow_updi_disable)
{
    int s;
    const UpdiDeviceMap *dev = updi_get_device();

    if (nvm_check_window("lockbits", addr, len,
                         dev->lock_base, dev->lock_size) < 0)
        return -1;

    /* Safety interlock A: chip must be in the post-erase state.
     * AVR-Dx requires the lock bytes to be programmed only after a
     * CHIPERASE has cleared the LOCKSTATUS flag (ASI_SYS_STATUS bit 1).
     * If LOCKSTATUS is asserted we refuse the write. */
    s = updi_ldcs(fd, ASI_SYS_STATUS);
    if (s < 0) {
        fprintf(stderr, "nvm_lockbits_write: SYS_STATUS read failed\n");
        return -1;
    }
    if (s & 0x02u) {
        fprintf(stderr,
                "nvm_lockbits_write: refusing — LOCKSTATUS asserted; run "
                "--erase first (SYS_STATUS=0x%02x)\n", s & 0xFF);
        return UPDI_ERR_LOCKED;
    }

    /* Safety interlock B: refuse to program any value that disables the
     * UPDI host link, unless the caller has explicitly opted in via
     * --allow-lock-updi.  The only safe pattern is UPDI_LOCK_UNLOCKED
     * (little-endian on the wire). */
    if (!allow_updi_disable && len == 4u) {
        uint32_t v = (uint32_t)data[0]       |
                     ((uint32_t)data[1] << 8) |
                     ((uint32_t)data[2] << 16) |
                     ((uint32_t)data[3] << 24);
        if (v != UPDI_LOCK_UNLOCKED) {
            fprintf(stderr,
                    "nvm_lockbits_write: refusing — value 0x%08x would "
                    "disable UPDI; pass --allow-lock-updi to override\n",
                    (unsigned)v);
            return UPDI_ERR_LOCKED;
        }
    }

    return nvm_eeprom_write_bytes(fd, addr, data, len, "lockbits");
}

int updi_console_poll(int fd, char *buf, size_t cap)
{
    fd_set         rfds;
    struct timeval tv = { 0, 0 };   /* zero timeout — non-blocking */
    int            sel;
    ssize_t        n;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    sel = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sel < 0)
        return -1;
    if (sel == 0)
        return 0;

    n = read(fd, buf, cap);
    if (n < 0)
        return -1;
    return (int)n;
}

/* ── Device-signature diagnostics (Phase 7, LLR-UPDI-13) ──────────────── *
 *
 * updi_read_device_info() reads:
 *   - SIGROW.DEVICEID0..2 @ 0x1100-0x1102   (datasheet §7.6.1, Table 7-4)
 *   - SYSCFG.REVID        @ 0x0F01          (datasheet §8.3.2.1; SYSCFG
 *                                            base 0x0F00 per memory map)
 *   - SIGROW.SERNUM0..15  @ 0x1110-0x111F   (datasheet §7.6.2.3: 16 bytes)
 *   - ASI_SYS_STATUS, ASI_KEY_STATUS, ASI_STATUSB via LDCS
 *
 * The operation is non-destructive: it does not halt the CPU and does
 * not initiate any NVM activity. On the first UPDI read failure it sets
 * info->fail_op to a short ASCII tag identifying the failed step and
 * returns -1.                                                             */
int updi_read_device_info(int fd, UpdiDeviceInfo *info)
{
    int v;

    if (info == NULL)
        return -1;

    info->fail_op    = NULL;
    info->fail_errno = 0;

    if (updi_mem_read(fd, 0x1100u, info->device_id, 3u) < 0) {
        info->fail_op    = "sigrow";
        info->fail_errno = -1;
        return -1;
    }
    if (updi_mem_read(fd, 0x0F01u, &info->revid, 1u) < 0) {
        info->fail_op    = "revid";
        info->fail_errno = -1;
        return -1;
    }
    if (updi_mem_read(fd, 0x1110u, info->serial, 16u) < 0) {
        info->fail_op    = "sernum";
        info->fail_errno = -1;
        return -1;
    }

    v = updi_ldcs(fd, ASI_SYS_STATUS);
    if (v < 0) {
        info->fail_op    = "sys-status";
        info->fail_errno = -1;
        return -1;
    }
    info->asi_sys_status = (uint8_t)v;

    v = updi_ldcs(fd, ASI_KEY_STATUS);
    if (v < 0) {
        info->fail_op    = "key-status";
        info->fail_errno = -1;
        return -1;
    }
    info->asi_key_status = (uint8_t)v;

    v = updi_ldcs(fd, ASI_STATUSB);
    if (v < 0) {
        info->fail_op    = "asi-statusb";
        info->fail_errno = -1;
        return -1;
    }
    info->asi_statusb = (uint8_t)v;

    return 0;
}

/* ── Phase 9 — read-back verify (LLR-UPDI-30) ────────────────────────── *
 *
 * updi_nvm_read() is a thin verify-friendly wrapper around the existing
 * updi_mem_read() burst path.  It exists so the verify orchestrator in
 * src/main.c (verify_segments()) need not directly reach into the
 * memory primitives — see SDD §2.1 layering rule "NVM reads live in
 * src/updi.c".  No NVM commands are issued; the read is non-destructive
 * and the CPU is not halted.                                            */
int updi_nvm_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    return updi_mem_read(fd, addr, buf, len);
}

/* CRC-32 (IEEE 802.3 polynomial), table-free byte-wise implementation.
 * Used by verify_segments() to summarise expected-vs-actual page
 * contents in 8 hex characters.                                        */
uint32_t updi_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)buf[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)0 - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ── Phase 9 — auto-baud / link-quality probe (LLR-UPDI-31) ──────────── *
 *
 * Walk a fixed candidate-baud ladder from fastest to slowest.  At each
 * rung, open UPDI (which performs the full cold-start handshake and
 * SIB read), then issue `samples` read-only LDCS probes on
 * ASI_STATUSA.  STATUSA carries UPDIREV in its upper nibble and is
 * always readable while UPDI is enabled (see updi_open()'s cold-start
 * comment); a probe error is any LDCS failure or any response byte
 * whose upper nibble is 0x0 (which would mean the link is not
 * synchronised).
 *
 * The function reports the per-rung result to the caller via the
 * visitor callback (so callers can render a baud=<N> errors=<K>/<total>
 * line) and returns the highest rung that achieved zero errors.  On
 * total failure (every rung failed to even open) returns -1.          */
const int updi_baud_ladder[] = {
    230400, 200000, 150000, 115200, 57600, 38400, 19200,
};
const size_t updi_baud_ladder_count =
    sizeof(updi_baud_ladder) / sizeof(updi_baud_ladder[0]);

void updi_set_nvm_progress(UpdiNvmProgressCb cb, void *user)
{
    g_nvm_progress_cb   = cb;
    g_nvm_progress_user = user;
}

int updi_probe_baud(const char *serial_device,
                    int samples,
                    UpdiBaudReport report, void *user)
{
    int best = -1;

    if (serial_device == NULL || samples <= 0)
        return -1;

    for (size_t i = 0; i < updi_baud_ladder_count; i++) {
        int baud = updi_baud_ladder[i];
        int fd = updi_open(serial_device, baud);
        if (fd < 0) {
            if (report != NULL)
                report(baud, samples, samples, user);
            continue;
        }

        int errors = 0;
        for (int s = 0; s < samples; s++) {
            int v = updi_ldcs(fd, ASI_STATUSA);
            if (v < 0 || (v & 0xF0u) == 0u)
                errors++;
        }

        updi_close(fd);
        if (report != NULL)
            report(baud, errors, samples, user);

        if (errors == 0 && best < 0)
            best = baud;
    }

    return best;
}

/* ── Phase 9 — fuse pretty-printer (LLR-UPDI-32) ────────────────────── *
 *
 * The decoder is table-driven and family-aware.  The fuse layout is
 * identical across AVR-DA / AVR-DB / AVR-DD / AVR-DU / AVR-SD at the
 * FUSES window base — they share the AVR-Dx fuse map per datasheet
 * §6 (Memories) / §8 (FUSE) — so a single descriptor table covers
 * every family currently in g_device_table[].  Unknown bytes are
 * still printed as `raw=0x..` so future families that introduce new
 * fuses degrade gracefully.                                          */

/* Each AVR-Dx fuse byte is a packed bitfield.  We describe each named
 * field as `{ name, byte-offset-in-FUSES-window, low-bit, width }`.
 * The decoder picks each field out and renders `<name>=<value>`.    */
typedef struct {
    const char *name;
    uint8_t     byte;
    uint8_t     shift;
    uint8_t     width;
} FuseField;

/* AVR-Dx FUSES window (16 bytes at offset 0x1050).
 * Source: AVR128DA datasheet §8.5 (Configuration and User Fuses). */
static const FuseField g_avr_dx_fuses[] = {
    /* 0x00 WDTCFG : PERIOD[3:0], WINDOW[7:4]                       */
    { "WDTCFG.PERIOD",     0x00, 0, 4 },
    { "WDTCFG.WINDOW",     0x00, 4, 4 },
    /* 0x01 BODCFG : SLEEP[1:0], ACTIVE[3:2], SAMPFREQ[4], LVL[7:5] */
    { "BODCFG.SLEEP",      0x01, 0, 2 },
    { "BODCFG.ACTIVE",     0x01, 2, 2 },
    { "BODCFG.SAMPFREQ",   0x01, 4, 1 },
    { "BODCFG.LVL",        0x01, 5, 3 },
    /* 0x02 OSCCFG : CLKSEL[2:0], OSCHFFRQ[3]                       */
    { "OSCCFG.CLKSEL",     0x02, 0, 3 },
    { "OSCCFG.OSCHFFRQ",   0x02, 3, 1 },
    /* 0x05 SYSCFG0 : EESAVE[0], RSTPINCFG[3:2], CRCSEL[5], CRCSRC[7:6] */
    { "SYSCFG0.EESAVE",    0x05, 0, 1 },
    { "SYSCFG0.RSTPINCFG", 0x05, 2, 2 },
    { "SYSCFG0.CRCSEL",    0x05, 5, 1 },
    { "SYSCFG0.CRCSRC",    0x05, 6, 2 },
    /* 0x06 SYSCFG1 : SUT[2:0], MVSYSCFG[4:3]                       */
    { "SYSCFG1.SUT",       0x06, 0, 3 },
    { "SYSCFG1.MVSYSCFG",  0x06, 3, 2 },
    /* 0x07 CODESIZE: 8 bits                                        */
    { "CODESIZE",          0x07, 0, 8 },
    /* 0x08 BOOTSIZE: 8 bits                                        */
    { "BOOTSIZE",          0x08, 0, 8 },
};

static int fmt_append(char *out, size_t cap, size_t *off, const char *fmt, ...)
{
    if (*off >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off) return -1;
    *off += (size_t)n;
    return 0;
}

int updi_format_fuses(char *out, size_t cap,
                      const uint8_t *raw,  size_t raw_len,
                      const uint8_t *lock, size_t lock_len)
{
    if (out == NULL || cap == 0 || raw == NULL)
        return -1;

    const UpdiDeviceMap *dev = updi_get_device();
    size_t off = 0;

    if (fmt_append(out, cap, &off,
                   "Fuses (%s, %zu bytes):\n",
                   (dev->family ? dev->family : "?"), raw_len) < 0)
        return -1;

    for (size_t i = 0;
         i < sizeof(g_avr_dx_fuses) / sizeof(g_avr_dx_fuses[0]);
         i++) {
        const FuseField *f = &g_avr_dx_fuses[i];
        if (f->byte >= raw_len) continue;
        uint8_t  byte = raw[f->byte];
        uint8_t  mask = (uint8_t)(((1u << f->width) - 1u) << f->shift);
        uint8_t  val  = (uint8_t)((byte & mask) >> f->shift);
        if (fmt_append(out, cap, &off,
                       "  0x%02X  %-22s = 0x%02X\n",
                       (unsigned)f->byte, f->name, (unsigned)val) < 0)
            return -1;
    }

    /* Any bytes the descriptor table did not cover are shown raw so
     * unfamiliar layouts (e.g. a future AVR-Dx revision adding a new
     * fuse offset) are still visible to the operator.                 */
    bool covered[16] = { false };
    for (size_t i = 0;
         i < sizeof(g_avr_dx_fuses) / sizeof(g_avr_dx_fuses[0]);
         i++) {
        if (g_avr_dx_fuses[i].byte < sizeof(covered) / sizeof(covered[0]))
            covered[g_avr_dx_fuses[i].byte] = true;
    }
    for (size_t i = 0; i < raw_len && i < sizeof(covered)/sizeof(covered[0]); i++) {
        if (!covered[i]) {
            if (fmt_append(out, cap, &off,
                           "  0x%02X  %-22s = raw=0x%02X\n",
                           (unsigned)i, "(reserved)",
                           (unsigned)raw[i]) < 0)
                return -1;
        }
    }

    if (lock != NULL && lock_len >= 4) {
        uint32_t v = (uint32_t)lock[0] |
                     ((uint32_t)lock[1] << 8) |
                     ((uint32_t)lock[2] << 16) |
                     ((uint32_t)lock[3] << 24);
        const char *state = (v == UPDI_LOCK_UNLOCKED) ? "UNLOCKED" : "LOCKED";
        if (fmt_append(out, cap, &off,
                       "Lock: 0x%08X (%s)\n",
                       (unsigned)v, state) < 0)
            return -1;
    }

    return (int)off;
}
