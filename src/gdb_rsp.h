/* src/gdb_rsp.h — GDB Remote Serial Protocol server */
#ifndef AOD_GDB_RSP_H
#define AOD_GDB_RSP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

#include "elf_parser.h"
#include "fsm_mapper.h"

#define RSP_PACKET_MAX      2048
#define RSP_MAX_BREAKPOINTS 16

/* AVR BREAK opcode (little-endian word written into FLASH for software bps) */
#define AVR_BREAK_OPCODE    0x9598u

/* Stop-reason: SIGTRAP (signal 5) is what GDB expects after halt/step. */
#define RSP_STOP_SIGTRAP    "T05"

struct RspHandlers;

/* Shared session state passed to every default handler as ctx. */
typedef struct {
    int                       updi_fd;
    int                      *gdb_fd_p;     /* pointer to caller's gdb client fd */
    FsmContext               *fsm;
    const AvrOsSymbolIndex   *idx;
    int                      *g_thread_p;   /* selected thread for g/G/P */
    int                      *c_thread_p;   /* selected thread for c/s */
    volatile sig_atomic_t    *quit_p;       /* k packet sets *quit_p = 1 */
} RspContext;

/* Each handler returns 0 on success or -1 on error. The handler is
 * responsible for sending its own RSP reply via rsp_send_packet(). */
typedef int (*RspHandlerFn)(int fd, const char *packet, void *ctx);

typedef struct RspHandlers {
    RspHandlerFn on_halt_reason;    /* ?                              */
    RspHandlerFn on_read_regs;      /* g                              */
    RspHandlerFn on_write_regs;     /* G ; also P (single register)   */
    RspHandlerFn on_read_mem;       /* m addr,len                     */
    RspHandlerFn on_write_mem;      /* M addr,len:data ; X (binary)   */
    RspHandlerFn on_continue;       /* c , vCont;c[:tid]              */
    RspHandlerFn on_step;           /* s , vCont;s[:tid]              */
    RspHandlerFn on_insert_bp;      /* Z0 addr,kind                   */
    RspHandlerFn on_remove_bp;      /* z0 addr,kind                   */
    RspHandlerFn on_thread_info;    /* qfThreadInfo / qsThreadInfo    */
    RspHandlerFn on_thread_extra;   /* qThreadExtraInfo,<tid>         */
    RspHandlerFn on_set_thread_g;   /* Hg<tid>                        */
    RspHandlerFn on_set_thread_c;   /* Hc<tid>                        */
    RspHandlerFn on_monitor;        /* qRcmd,<hex>                    */
    RspHandlerFn on_detach;         /* D , k                          */
    void        *ctx;
} RspHandlers;

/* ── Lifecycle ───────────────────────────────────────────────────────── */
int  rsp_listen(uint16_t port);
int  rsp_accept(int listen_fd);
void rsp_close (int fd);

/* ── Packet codec ────────────────────────────────────────────────────── */
int  rsp_recv_packet(int fd, char *buf, size_t cap);
int  rsp_send_packet(int fd, const char *payload);

/* ── No-ack mode (toggled by QStartNoAckMode) ────────────────────────── */
void rsp_set_noack(bool enabled);
bool rsp_get_noack(void);

/* ── Dispatch ────────────────────────────────────────────────────────── */
int  rsp_dispatch(int fd, const char *packet, RspHandlers *h);

/* ── Default handlers (used by main; tests may override individually) ── */
void rsp_default_handlers(RspHandlers *h, RspContext *ctx);

#endif /* AOD_GDB_RSP_H */
