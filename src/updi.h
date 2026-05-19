/* src/updi.h */
#ifndef AOD_UPDI_H
#define AOD_UPDI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define UPDI_SYNCH       0x55
#define UPDI_ACK         0x40
#define UPDI_MAX_BLOCK   256
#define UPDI_BREAK_BAUD  300
#define UPDI_ERR_WP      (-2)
#define UPDI_ERR_LOCKED  (-3)
#define UPDI_FLASH_PAGE_SIZE 512u
/* AVR-Dx unified-UPDI memory map: FLASH section base.  Add to a
 * program-memory byte offset (avr-gcc .text VMA) to obtain the UPDI
 * physical address used by ST_PTR_LONG.  Section selection within FLASH
 * (>32 KiB parts) is done via NVMCTRL.CTRLB.FLMAP. */
#define UPDI_FLASH_BASE      0x800000u

/* AVR-Dx UPDI data-space addresses for non-FLASH NVM windows (Phase 8 —
 * see HLR-046).  These are the SILICON addresses presented to the UPDI
 * STS / LDS opcodes — i.e. what NVMCTRL actually decodes.  They are NOT
 * the 0x8x0000-prefixed ELF VMAs that avr-gcc emits in `.eeprom`,
 * `.user_signatures`, `.fuse`, `.lock`, `.signature` sections; see
 * ELF_VMA_* below and `load_segments()` in main.c for the translation.
 *
 * Values cross-checked against Microchip pymcuprog device data for
 * avr128da28 (Microchip.AVR-Dx_DFP 2.0.137).  Sizes are AVR128DA28
 * defaults; the writers use these as upper bounds so over-large segments
 * are rejected rather than silently truncated.                          */
#define UPDI_EEPROM_BASE     0x001400u
#define UPDI_EEPROM_SIZE     0x000200u  /* 512 B (AVR128DA28)            */
#define UPDI_USERROW_BASE    0x001080u
#define UPDI_USERROW_SIZE    0x000020u  /* 32 B                          */
#define UPDI_FUSES_BASE      0x001050u
#define UPDI_FUSES_SIZE      0x000010u  /* 16 B (9 used; rounded up)     */
#define UPDI_LOCK_BASE       0x001040u
#define UPDI_LOCK_SIZE       0x000004u  /* 4 lock bytes                  */
#define UPDI_SIGROW_BASE     0x001100u
#define UPDI_SIGROW_SIZE     0x000040u  /* 64 B signature row            */

/* avr-libc AVR-Dx (avrxmega3 family) linker-script VMAs for non-FLASH
 * ELF sections.  `load_segments()` recognises p_vaddr values in these
 * bands and translates them to the corresponding silicon UPDI address
 * (UPDI_*_BASE + offset-within-band) before dispatching to the writer.
 * These bands are an ELF/toolchain convention, not silicon addresses. */
#define ELF_VMA_EEPROM       0x810000u
#define ELF_VMA_FUSES        0x820000u
#define ELF_VMA_LOCK         0x830000u
#define ELF_VMA_SIGROW       0x840000u
#define ELF_VMA_USERROW      0x850000u
#define ELF_VMA_BAND_MASK    0xFF0000u  /* top byte selects the band     */
#define ELF_VMA_OFFSET_MASK  0x00FFFFu  /* low 16 bits are offset-in-band*/

/* Lockbit value that disables UPDI access from the host (UPDIDIS pattern,
 * AVR-Dx datasheet §6.3 — lockbits programmed to anything OTHER than
 * 0x5CC5C55C disable UPDI on the next reset).  The conservative interlock
 * in updi_nvm_write_lockbits() refuses any value that is NOT the unlock
 * pattern unless --allow-lock-updi is set.                              */
#define UPDI_LOCK_UNLOCKED   0x5CC5C55Cu

/* SIB (System Information Block) read opcode: KEY family with SIB-direction
 * bit set and size selector = 32 bytes.  Issuing this opcode also wakes a
 * UPDI target from sleep (matching avrdude's serialupdi behaviour). */
#define UPDI_OP_KEY_SIB     0xE6
#define UPDI_SIB_LEN        32

/* UPDI CS-space register offsets (datasheet §35.4) */
#define ASI_STATUSA     0x00     /* RO: UPDIREV[7:4]                         */
#define ASI_STATUSB     0x01     /* RO: PESIG error signature                */
#define ASI_CTRLA       0x02     /* RW: IBDLY, PARD, DTD, RSD, GTVAL         */
#define ASI_CTRLB       0x03     /* RW: NACKDIS, CCDETDIS, UPDIDIS           */
#define ASI_KEY_STATUS  0x07     /* RO: UROWWRITE, NVMPROG, CHIPER           */
#define ASI_RESET_REQ   0x08     /* RW: RSTREQ[7:0]  (0x59 = reset, 0 = run) */
#define ASI_SYS_CTRLA   0x0A     /* RW: UROWDONE (b1), CLKREQ (b0)           */
#define ASI_SYS_STATUS  0x0B     /* RO: ERASEFAIL/SYSRST/INSLEEP/NVMPROG/... */

/* ASI_CTRLA bits (datasheet §35.4.2) */
#define ASI_CTRLA_IBDLY     0x80    /* Inter-Byte Delay enable                */

/* ASI_CTRLB bits (datasheet §35.4.3) */
#define ASI_CTRLB_CCDETDIS  0x08    /* Collision/Contention Detection Disable */

/* ── OCD ASI registers (community-derived; see doc/reference/guesswork.md) ─
 * Accessed via UPDI LDCS/STCS.                                              */
#define ASI_OCD_CTRLA   0x04u   /* RW: SOR_DIS, RUN, STOP                    */
#define ASI_OCD_STATUS  0x05u   /* RO: OCDMV, STOPPED                        */
#define ASI_OCD_MESSAGE 0x0Du   /* RW: 8-bit msg channel (host↔target)       */

/* ASI_OCD_CTRLA bits */
#define ASI_OCD_CTRLA_STOP     0x01u   /* halt CPU                           */
#define ASI_OCD_CTRLA_RUN      0x02u   /* resume CPU                         */
#define ASI_OCD_CTRLA_SOR_DIS  0x80u   /* stop-on-reset disable              */

/* ASI_OCD_STATUS bits */
#define ASI_OCD_STATUS_STOPPED 0x01u   /* CPU is halted                      */
#define ASI_OCD_STATUS_OCDMV   0x10u   /* OCD message valid                  */

/* ── OCD memory-mapped peripheral (base 0x0F80) ──────────────────────────
 * Accessed via UPDI LDS/STS.  Layout per guesswork.md (AVR-Dx, OCD v1).    */
#define OCD_BASE        0x0F80u
#define OCD_BP0A        (OCD_BASE + 0x00u)  /* 3 bytes: BP0 byte address    */
#define OCD_BP1A        (OCD_BASE + 0x04u)  /* 3 bytes: BP1 byte address    */
#define OCD_CTRL0       (OCD_BASE + 0x08u)  /* PCHOLD/HWBP/STEP             */
#define OCD_CTRL1       (OCD_BASE + 0x09u)  /* BP0/BP1/EXTBRK/SWBP/JMP/INT  */
#define OCD_STATUS0     (OCD_BASE + 0x0Cu)  /* STOPPED/EXT/RESET            */
#define OCD_STATUS1     (OCD_BASE + 0x0Du)  /* BP0_STEP/BP1/SWBP/...        */
#define OCD_INSN0       (OCD_BASE + 0x10u)  /* injected insn word 0         */
#define OCD_INSN1       (OCD_BASE + 0x12u)  /* injected insn word 1         */
#define OCD_PC          (OCD_BASE + 0x14u)  /* word-addr PC+1 (16-bit)      */
#define OCD_SP          (OCD_BASE + 0x18u)  /* SP (16-bit)                  */
#define OCD_SREG        (OCD_BASE + 0x1Cu)  /* SREG                         */
#define OCD_REGFILE     (OCD_BASE + 0x20u)  /* r0..r31 = base+0x00..0x1F    */

/* OCD CTRL0 bits */
#define OCD_CTRL0_PCHOLD  0x01u   /* hold PC during instruction injection   */
#define OCD_CTRL0_HWBP    0x02u   /* global hardware-breakpoint enable      */
#define OCD_CTRL0_STEP    0x04u   /* single-step armed                      */

/* OCD CTRL1 bits */
#define OCD_CTRL1_BP0     0x01u   /* breakpoint 0 enable                    */
#define OCD_CTRL1_BP1     0x02u   /* breakpoint 1 enable                    */
#define OCD_CTRL1_EXTBRK  0x10u   /* halt on EXTBRK pin                     */
#define OCD_CTRL1_SWBP    0x20u   /* halt on BREAK opcode (read-only set)   */
#define OCD_CTRL1_JMP     0x40u   /* halt after change-of-flow              */
#define OCD_CTRL1_INT     0x80u   /* halt on interrupt vector entry         */

/* OCD STATUS0/STATUS1 bits (read-only halt cause) */
#define OCD_STATUS0_STOPPED 0x04u
#define OCD_STATUS0_EXT     0x40u
#define OCD_STATUS0_RESET   0x80u
#define OCD_STATUS1_BP0STEP 0x01u
#define OCD_STATUS1_BP1     0x02u
#define OCD_STATUS1_EXTBRK  0x10u
#define OCD_STATUS1_SWBP    0x20u
#define OCD_STATUS1_JMP     0x40u
#define OCD_STATUS1_INT     0x80u

int  updi_open(const char *device, int baud);
void updi_close(int fd);
int  updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len);
int  updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len);

/* OCD / debug control (CPU run state).  Available once updi_enter_debug()
 * has put the target into OCD mode.                                       */
int  updi_enter_debug(int fd);
int  updi_halt(int fd);
int  updi_run(int fd);
int  updi_step(int fd);
int  updi_ocd_poll_halted(int fd, int timeout_ms);
int  updi_ocd_read_halt_status(int fd, uint8_t *st0, uint8_t *st1);

/* OCD register-file access (CPU must be halted). */
int  updi_ocd_read_gpr (int fd, uint8_t n, uint8_t *val);
int  updi_ocd_write_gpr(int fd, uint8_t n, uint8_t val);
int  updi_ocd_read_pc  (int fd, uint32_t *byte_addr);
int  updi_ocd_write_pc (int fd, uint32_t byte_addr);
int  updi_ocd_read_sp  (int fd, uint16_t *val);
int  updi_ocd_write_sp (int fd, uint16_t val);
int  updi_ocd_read_sreg(int fd, uint8_t *val);
int  updi_ocd_write_sreg(int fd, uint8_t val);

/* Hardware breakpoints. idx is 0 or 1. */
int  updi_ocd_set_hw_bp  (int fd, int idx, uint32_t byte_addr);
int  updi_ocd_clear_hw_bp(int fd, int idx);

int  updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data, size_t len);
int  updi_nvm_flash_patch(int fd, uint32_t addr, const uint8_t *data, size_t len);

/* ── Phase 8 — non-FLASH NVM programming (HLR-046, HLR-047) ─────────── */
/* All four entry points expect addresses in the unified 24-bit UPDI
 * address space (e.g. `UPDI_EEPROM_BASE + offset`).  They enter NVMPROG
 * mode if not already entered (idempotent), program the requested bytes
 * via NVMCTRL.CMD = EEERWR (0x13) with per-byte BUSY-clear polling, and
 * return 0 on success or -1 on any failure.                            */
int  updi_nvm_write_eeprom  (int fd, uint32_t addr, const uint8_t *data, size_t len);
int  updi_nvm_write_userrow (int fd, uint32_t addr, const uint8_t *data, size_t len);
int  updi_nvm_write_fuses   (int fd, uint32_t addr, const uint8_t *data, size_t len);

/* Lockbits.  Returns `UPDI_ERR_LOCKED` if (a) the device is not in a
 * post-chip-erase state (ASI_SYS_STATUS.LOCKSTATUS = 0 prerequisite is
 * violated), or (b) the caller did not pass `allow_updi_disable` and the
 * 4-byte payload is not the UPDI-unlock pattern (UPDI_LOCK_UNLOCKED).  */
int  updi_nvm_write_lockbits(int fd, uint32_t addr, const uint8_t *data, size_t len,
                             bool allow_updi_disable);

int  updi_chip_erase(int fd);
int  updi_console_poll(int fd, char *buf, size_t cap);

/* ── Device-signature diagnostics (Phase 7) ──────────────────────────── */
typedef struct {
    uint8_t     device_id[3];   /* SIGROW DEVICEID0..2 @ 0x1100-0x1102      */
    uint8_t     revid;          /* SYSCFG.REVID        @ 0x0F01 (§8.3.2.1)  */
    uint8_t     serial[16];     /* SIGROW SERNUM0..15  @ 0x1110-0x111F (§7.6) */
    uint8_t     asi_sys_status;
    uint8_t     asi_key_status;
    uint8_t     asi_statusb;
    const char *fail_op;        /* NULL on success                     */
    int         fail_errno;     /* negative updi error on failure      */
} UpdiDeviceInfo;

int  updi_read_device_info(int fd, UpdiDeviceInfo *info);

/* ── Phase 9 — read-back verify (HLR-PHASE9-VERIFY, LLR-UPDI-30) ─────
 * Thin wrapper around updi_mem_read() with a verify-friendly contract:
 * reads `len` bytes from the unified-address-space `addr` into the
 * caller-supplied buffer, no NVM activity initiated.  Used by
 * verify_segments() in src/main.c after every NVM write to compare
 * silicon contents against the ELF payload.  Returns 0 on success,
 * -1 on any UPDI read failure.                                       */
int  updi_nvm_read(int fd, uint32_t addr, uint8_t *buf, size_t len);

/* CRC-32 (IEEE 802.3 polynomial 0xEDB88320, init 0xFFFFFFFF, output
 * inverted).  Pure host-side helper used by verify_segments() to
 * summarise expected-vs-actual page contents in a compact form.     */
uint32_t updi_crc32(const uint8_t *buf, size_t len);

/* ── Phase 9 — auto-baud / link-quality probe (HLR-PHASE9-AUTOBAUD,
 *    LLR-UPDI-31) ────────────────────────────────────────────────────
 * Walk a fixed candidate-baud ladder (highest → lowest), opening
 * UPDI at each rate and issuing a small fixed sequence of read-only
 * LDCS probes (ASI_STATUSA × `samples`).  For each rung, reports the
 * error count to the caller via the visitor callback (NULL skips
 * reporting), and returns the highest baud that achieved zero errors
 * over the sample window.  Strictly read-only: no STS/NVM/SYSRST
 * writes are performed.  On total failure returns -1.
 *
 * `serial_device` is opened and closed inside this function; the
 * caller should NOT pass an already-open fd.                        */
typedef void (*UpdiBaudReport)(int baud, int errors, int samples, void *user);
int  updi_probe_baud(const char *serial_device,
                     int samples,
                     UpdiBaudReport report, void *user);

/* Candidate-baud ladder used by updi_probe_baud().  Exposed so tests
 * and the `--device` printer can iterate the same list.             */
extern const int updi_baud_ladder[];
extern const size_t updi_baud_ladder_count;

/* ── Phase 9 — fuse pretty-printer (HLR-PHASE9-FUSES, LLR-UPDI-32) ───
 * Format the raw fuse bytes for the currently-selected device family
 * (per `updi_get_device()`) into a human-readable multi-line buffer
 * styled after `avrdude -Tu` output.  `raw[fuses_size]` is the
 * caller-supplied FUSE-window snapshot read via updi_nvm_read().
 * `lock[lock_size]` is the LOCK-window snapshot (4 bytes on every
 * supported family).  Decoded bit-fields use the per-family fuse
 * descriptor table next to g_device_table[].  Unknown bytes render
 * as `raw=0x..`.  Returns bytes written (excluding NUL) or -1 if
 * the output buffer is too small.                                  */
int  updi_format_fuses(char *out, size_t cap,
                       const uint8_t *raw,  size_t raw_len,
                       const uint8_t *lock, size_t lock_len);

/* Reserved process exit code for read-back verify failure
 * (LLR-MAIN-14).  Distinct from 1 (I/O / config failure) so CI
 * pipelines can disambiguate.                                      */
#define UPDI_EXIT_VERIFY_FAIL 2

/* ── Multi-family runtime device map ──────────────────────────────────
 * The compile-time `UPDI_*_BASE/_SIZE` macros above are the AVR-DA/DB
 * defaults retained for backward compatibility (Unity tests still refer
 * to them).  At runtime, `updi_select_device()` (called from main.c
 * just after `updi_open()`) replaces these with a per-family map drawn
 * from the static table in `updi.c`.  All NVM writers in updi.c and the
 * ELF-band translator in main.c read the active map through
 * `updi_get_device()`.
 *
 * The five supported families share the NVMCTRL v2 command set
 * (FLWR=0x02, FLPER=0x08, EEERWR=0x13, CHER=0x20) and the same UPDI
 * USERROW key sequence.  AVR-EA/AVR-EB use NVMCTRL v3 (different CMD
 * bytes) and are intentionally NOT in the table — they will be added
 * once their command set is wired up.
 *
 * Hardware-validation status: AVR-DA is exercised on bench silicon by
 * the hw-test target (E1/E2/E3).  AVR-DB/DD/DU/SD are declared from
 * datasheet evidence only and require `--force-device=<family>` to
 * use, since their SIGROW location or memory map deviates from the
 * autodetect probe path.                                              */
typedef struct {
    const char *family;          /* "AVR-DA", "AVR-DB", "AVR-DD",
                                  *  "AVR-DU", "AVR-SD"                  */
    uint32_t    userrow_base;
    uint32_t    userrow_size;    /* also the page/commit row length      */
    uint32_t    eeprom_base;
    uint32_t    eeprom_size;
    uint32_t    fuses_base;
    uint32_t    fuses_size;
    uint32_t    lock_base;
    uint32_t    lock_size;
    uint32_t    sigrow_base;
    bool        hw_tested;       /* true ⇔ exercised on bench silicon    */
} UpdiDeviceMap;

/* Maximum USERROW row length across all supported families.  Used to
 * size the stack buffer in `nvm_userrow_write()` so a single code path
 * handles all five families.                                          */
#define UPDI_USERROW_ROW_LEN_MAX  512u

const UpdiDeviceMap *updi_get_device(void);

/* Select the active per-family map.  If `force_family` is non-NULL it
 * must match an entry's `family` string (case-insensitive) and the
 * device is configured to that family without an autodetect probe.
 * Otherwise the function reads SIGROW.DEVICEID[0..2] @ 0x1100 and
 * matches against the autodetect list (currently AVR-DA / AVR-DB
 * SIGROWs since those families place SIGROW at 0x1100).  Returns 0 on
 * success, -1 with a diagnostic on the failure paths:
 *
 *   • unknown / unreadable SIGROW with no `--force-device`,
 *   • `force_family` does not match any entry in the table,
 *   • family identified as NVMCTRL v3 (AVR-EA / AVR-EB) — not yet
 *     supported.                                                       */
int  updi_select_device(int fd, const char *force_family);

/* ── Part-name → family classifier ────────────────────────────────────
 * Maps a lowercase device-name string (e.g. "avr128da28", "avr64dd32",
 * obtained from the ELF's `.note.gnu.avr.deviceinfo` note) onto the
 * canonical family string used by `g_device_table[]` ("AVR-DA",
 * "AVR-DB", "AVR-DD", "AVR-DU", "AVR-SD").  The classifier lives in
 * `updi.c` because it is logically part of the device-table layer:
 * adding a new family must update both the table and this mapping
 * together.
 *
 * Returns the family string (statically allocated) on a match, or
 * NULL when `partname` is NULL, empty, or does not match any known
 * Microchip AVR-Dx/Du/Sd naming pattern.  The caller (main.c) treats
 * NULL as "ELF carries no usable device identification" and falls
 * back to SIGROW autodetect.                                          */
const char *updi_family_from_partname(const char *partname);

#endif /* AOD_UPDI_H */
