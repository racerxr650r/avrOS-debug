/* src/updi.h */
#ifndef AOD_UPDI_H
#define AOD_UPDI_H

#include <stddef.h>
#include <stdint.h>

#define UPDI_SYNCH       0x55
#define UPDI_ACK         0x40
#define UPDI_MAX_BLOCK   256
#define UPDI_BREAK_BAUD  300
#define UPDI_ERR_WP      (-2)
#define UPDI_FLASH_PAGE_SIZE 512u
/* AVR-Dx unified-UPDI memory map: FLASH section base.  Add to a
 * program-memory byte offset (avr-gcc .text VMA) to obtain the UPDI
 * physical address used by ST_PTR_LONG.  Section selection within FLASH
 * (>32 KiB parts) is done via NVMCTRL.CTRLB.FLMAP. */
#define UPDI_FLASH_BASE      0x800000u

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

#endif /* AOD_UPDI_H */
