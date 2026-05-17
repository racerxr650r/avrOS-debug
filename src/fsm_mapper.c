/* src/fsm_mapper.c — stub (Phase 3 will implement) */
#include "fsm_mapper.h"

int fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int updi_fd)
{
    (void)ctx; (void)idx; (void)updi_fd;
    return -1;
}

void fsm_invalidate(FsmContext *ctx)
{
    (void)ctx;
}

int fsm_get_active_thread(const FsmContext *ctx)
{
    (void)ctx;
    return -1;
}

int fsm_get_registers(const FsmContext *ctx, int thread_id, char *reg_buf)
{
    (void)ctx; (void)thread_id; (void)reg_buf;
    return -1;
}
