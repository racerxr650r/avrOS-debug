/* src/updi.c — stub (Phase 2 will implement) */
#include "updi.h"

int updi_open(const char *device, int baud)
{
    (void)device; (void)baud;
    return -1;
}

void updi_close(int fd)
{
    (void)fd;
}

int updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)
{
    (void)fd; (void)addr; (void)buf; (void)len;
    return -1;
}

int updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len)
{
    (void)fd; (void)addr; (void)buf; (void)len;
    return -1;
}

int updi_halt(int fd)
{
    (void)fd;
    return -1;
}

int updi_run(int fd)
{
    (void)fd;
    return -1;
}

int updi_step(int fd)
{
    (void)fd;
    return -1;
}

int updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data, size_t len)
{
    (void)fd; (void)word_addr; (void)data; (void)len;
    return -1;
}

int updi_console_poll(int fd, char *buf, size_t cap)
{
    (void)fd; (void)buf; (void)cap;
    return 0;
}
