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

int  updi_open(const char *device, int baud);
void updi_close(int fd);
int  updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len);
int  updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len);
int  updi_halt(int fd);
int  updi_run(int fd);
int  updi_step(int fd);
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
