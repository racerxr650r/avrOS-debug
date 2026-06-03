/* src/fsm_mapper.h */
#ifndef AOD_FSM_MAPPER_H
#define AOD_FSM_MAPPER_H

#include <stdbool.h>
#include <stdint.h>
#include "elf_parser.h"

#define FSM_MAX_THREADS 32
#define FSM_SYSTEM_THREAD_ID 1
#define FSM_FIRST_PSEUDO_THREAD_ID 2

typedef struct {
    int    gdb_id;
    char   name[32];
    uint32_t state_fn;
    bool   is_active;
} FsmThread;

typedef struct {
    FsmThread threads[FSM_MAX_THREADS];
    int       thread_count;
    int       active_id;
    bool      valid;
} FsmContext;

int  fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int updi_fd);
void fsm_invalidate(FsmContext *ctx);
int  fsm_get_active_thread(const FsmContext *ctx);
int  fsm_get_registers(const FsmContext *ctx, int thread_id, char *reg_buf);

#endif /* AOD_FSM_MAPPER_H */
