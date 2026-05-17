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

/* UPDI CS-space register offsets (datasheet §35.4) */
#define ASI_STATUSB     0x01     /* RO: PESIG error signature                */
#define ASI_CTRLA       0x02     /* RW: IBDLY, PARD, DTD, RSD, GTVAL         */
#define ASI_CTRLB       0x03     /* RW: NACKDIS, CCDETDIS, UPDIDIS           */
#define ASI_KEY_STATUS  0x07     /* RO: UROWWRITE, NVMPROG, CHIPER           */
#define ASI_RESET_REQ   0x08     /* RW: RSTREQ[7:0]  (0x59 = reset, 0 = run) */
#define ASI_SYS_CTRLA   0x0A     /* RW: UROWDONE (b1), CLKREQ (b0)           */
#define ASI_SYS_STATUS  0x0B     /* RO: ERASEFAIL/SYSRST/INSLEEP/NVMPROG/... */

int  updi_open(const char *device, int baud);
void updi_close(int fd);
int  updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len);
int  updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len);
int  updi_halt(int fd);
int  updi_run(int fd);
int  updi_step(int fd);
int  updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data, size_t len);
int  updi_console_poll(int fd, char *buf, size_t cap);

#endif /* AOD_UPDI_H */
