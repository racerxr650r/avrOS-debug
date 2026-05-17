/* src/gdb_rsp.c — stub (Phase 4 will implement) */
#include "gdb_rsp.h"

int rsp_listen(int port)
{
    (void)port;
    return -1;
}

int rsp_accept(int listen_fd)
{
    (void)listen_fd;
    return -1;
}

void rsp_close(int fd)
{
    (void)fd;
}

int rsp_recv_packet(int fd, char *buf, size_t buf_size)
{
    (void)fd; (void)buf; (void)buf_size;
    return -1;
}

int rsp_send_packet(int fd, const char *payload)
{
    (void)fd; (void)payload;
    return -1;
}

int rsp_dispatch(int fd)
{
    (void)fd;
    return -1;
}
