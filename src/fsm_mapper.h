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
    int    gdb_id;        /* stable per-FSM index (display/order only)   */
    char   name[32];      /* FSM name (from descriptor name pointer)      */
    char   state_name[32];/* current state name (FSM currStateName), or
                           * "" when the FSM has not yet dispatched       */
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

#endif /* AOD_FSM_MAPPER_H */
