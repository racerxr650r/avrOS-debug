/* src/avros.h — avrOS runtime-table introspection (core layer).
 *
 * Reads the application's registered **events** and **queues** from the target
 * over UPDI background reads (non-intrusive — never halts the CPU), decoding the
 * avrOS descriptor tables located via the `AvrOsSymbolIndex`.  This is the one
 * place that logic lives: both client front-ends call it — the GDB-RSP
 * `monitor avros events|queues` formatter (`src/monitor.c`) and the DAP
 * `avrosdb/eventList|queueList` custom requests (`src/dap.c`) — so neither
 * front-end depends on the other.  (FSM-table introspection has the same role
 * and already lives in its own core module, `src/fsm_mapper.c`.)
 *
 * Layer: core (depends only on `src/updi.c` + `src/elf_parser.c`); below the
 * protocol/front-end layer.
 */
#ifndef AOD_AVROS_H
#define AOD_AVROS_H

#include <stdbool.h>
#include <stdint.h>

#include "elf_parser.h"   /* AvrOsSymbolIndex */

/* One registered avrOS event. */
typedef struct {
    char    name[32];     /* event name (followed from the descriptor pointer) */
    uint8_t status;       /* current status byte (0 when unreadable)           */
    bool    has_status;   /* false when the descriptor's status pointer is NULL */
} AvrosEvent;

/* One registered avrOS queue. */
typedef struct {
    uint16_t capacity;    /* queue capacity (elements) */
    uint16_t elem_size;   /* sizeOfElement (bytes)     */
} AvrosQueue;

/* Read up to `max` registered events / queues from the target into `out`.
 * Return the number written, 0 when none are registered (or `idx` is NULL), or
 * -1 on a UPDI read error.  Non-intrusive (background reads; no CPU halt). */
int avros_read_events(const AvrOsSymbolIndex *idx, int updi_fd,
                      AvrosEvent *out, int max);
int avros_read_queues(const AvrOsSymbolIndex *idx, int updi_fd,
                      AvrosQueue *out, int max);

#endif /* AOD_AVROS_H */
