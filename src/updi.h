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

/* ASI register offsets */
#define ASI_CTRLA       0x02
#define ASI_RESET_REQ   0x08
#define ASI_SYS_STATUS  0x0B
#define ASI_SYS_CTRL    0x0C

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
