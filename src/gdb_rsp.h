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
/* AVR-Dx OCD provides exactly two hardware breakpoint comparators
 * (BP0, BP1).  Both Z0 (software) and Z1 (hardware) GDB requests are
 * routed to the same two slots — see src/gdb_rsp.c dh_insert_bp.    */
#define RSP_MAX_BREAKPOINTS 2

/* HLR-055: monitor bp-mode values. */
#define RSP_BP_MODE_SW       0
#define RSP_BP_MODE_HW_ONLY  1

/* HLR-054: maximum number of simultaneous software breakpoints.  The
 * AVR architecture imposes no inherent limit (every two-byte FLASH
 * word can be patched independently) but the shadow table is a
 * fixed-size array to keep the RspContext POD-like and avoid heap
 * allocations on the fast path.  64 simultaneous SW BPs is well
 * beyond typical GDB use.                                            */
#define RSP_MAX_SW_BREAKPOINTS 64

/* HLR-054: per-session shadow entry for one software breakpoint. */
typedef struct {
    uint32_t addr;          /* GDB-side byte address (FLASH window)   */
    uint8_t  orig[2];       /* original 2-byte opcode, little-endian  */
    bool     in_use;
} RspSwBp;

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
    /* Shadow of the two AVR-Dx OCD hardware-breakpoint comparators.
     * Holds the GDB byte address currently installed in BP0/BP1, or
     * 0xFFFFFFFF for an empty slot.  Owned by gdb_rsp.c; main zeroes
     * it (designated initialiser leaves both = 0, then handler init
     * marks them empty on first use).                                */
    uint32_t                  hw_bp_addr[2];
    /* HLR-055: monitor verbs.  `allow_erase` mirrors the --allow-erase
     * CLI flag and gates `monitor erase` / `monitor chip-erase`.
     * `bp_mode` is 0 = "sw" (true SW BPs via FLASH BREAK, HLR-054) or
     * 1 = "hw-only" (legacy: Z0 aliases to HW comparators).  Mode is
     * mutated by `monitor bp-mode <sw|hw-only>` and persists for the
     * lifetime of the server process.                                */
    int                       allow_erase;
    int                       bp_mode;
    /* HLR-053: in-progress vFlash* transaction state.  `flash_xact_buf`
     * is malloc()'d on the first `vFlashErase` of a load sequence (or
     * extended by subsequent erases of contiguous ranges) and freed on
     * `vFlashDone` (success) or on any g/G/m/M/c/s packet that arrives
     * mid-transaction (abort).  NULL ⇔ no transaction in progress.    */
    uint8_t                  *flash_xact_buf;
    uint32_t                  flash_xact_base;   /* UPDI byte-addr      */
    size_t                    flash_xact_len;    /* bytes in buf        */
    /* HLR-054: per-session shadow of installed software breakpoints.
     * Empty slots have `in_use == false`.  Populated on Z0 (when the
     * server is in `bp_mode == RSP_BP_MODE_SW`, the default) and
     * drained on z0.  Cleared wholesale on `vFlashDone`,
     * `monitor reset`, and `monitor chip-erase`.                      */
    RspSwBp                   sw_bp[RSP_MAX_SW_BREAKPOINTS];
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
    RspHandlerFn on_query_c;        /* qC                             */
    RspHandlerFn on_query_offsets;  /* qOffsets                       */
    RspHandlerFn on_thread_alive;   /* T<tid>                         */
    RspHandlerFn on_restart;        /* R<XX>                          */
    RspHandlerFn on_vrun;           /* vRun;<args>                    */
    RspHandlerFn on_vattach;        /* vAttach;<pid>                  */
    RspHandlerFn on_vkill;          /* vKill;<pid>                    */
    RspHandlerFn on_vflash_erase;   /* vFlashErase:addr,length        */
    RspHandlerFn on_vflash_write;   /* vFlashWrite:addr:<binary>      */
    RspHandlerFn on_vflash_done;    /* vFlashDone                     */
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
/* Length-carrying variant.  Required for binary packets (vFlashWrite)
 * whose payload may contain embedded NUL bytes; the caller passes the
 * exact byte count returned by rsp_recv_packet().                      */
int  rsp_dispatch_n(int fd, const char *packet, size_t plen, RspHandlers *h);

/* ── Default handlers (used by main; tests may override individually) ── */
void rsp_default_handlers(RspHandlers *h, RspContext *ctx);

/* ── Helpers exposed for HLR-055 monitor verbs ───────────────────────── */
void rsp_hw_bp_clear_all(RspContext *ctx);

/* HLR-054: drop every SW-BP shadow entry (no silicon I/O — used by
 * the callers that have already destroyed the underlying FLASH).      */
void rsp_sw_bp_clear_all(RspContext *ctx);

#endif /* AOD_GDB_RSP_H */
