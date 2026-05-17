/* src/gdb_rsp.h */
#ifndef AOD_GDB_RSP_H
#define AOD_GDB_RSP_H

#include <stddef.h>

#define RSP_PACKET_MAX      2048
#define RSP_MAX_BREAKPOINTS 16

int  rsp_listen(int port);
int  rsp_accept(int listen_fd);
void rsp_close(int fd);
int  rsp_recv_packet(int fd, char *buf, size_t buf_size);
int  rsp_send_packet(int fd, const char *payload);
int  rsp_dispatch(int fd);

#endif /* AOD_GDB_RSP_H */
