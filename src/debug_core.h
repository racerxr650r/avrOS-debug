/* src/debug_core.h — Protocol-agnostic debug core (RSP + DAP seam).
 *
 * Single, documented include that aggregates the protocol-neutral verb surface
 * any client-facing protocol front-end drives to control and inspect the
 * target.  Today the only front-end is the GDB Remote Serial Protocol server
 * (src/gdb_rsp.c); a future native Debug Adapter Protocol (DAP) server is
 * intended to run *in parallel* over this same core (PVD §9; see
 * doc/reference/dual-protocol-architecture.md).
 *
 * The debug core owns everything that is NOT specific to a wire protocol:
 *   - execution control + register/memory access + the OCD primitives  (updi.h)
 *   - ELF/DWARF: symbols, flash-address translation, source-line lookup
 *                                                              (elf_parser.h)
 *   - target introspection: avrOS FSM task snapshots          (fsm_mapper.h)
 *   - the `monitor` verb dispatch                                (monitor.h)
 *
 * Layering contract (SDD §2.2 — HLR-073):
 *   - A protocol front-end translates wire framing <-> these verbs and is the
 *     ONLY layer that formats a protocol-specific reply.  The core never emits
 *     RSP, DAP, or any other wire encoding, and is never reached *into* from a
 *     lower layer (UPDI helpers never choose policy such as breakpoint slots).
 *   - The HW-comparator arbiter, the software-breakpoint shadow/step-over
 *     model, and the stop-cause classifier are core *policy* that currently
 *     still live inside gdb_rsp.c.  Relocating their bodies into a dedicated
 *     debug_core.c is deferred to the DAP-server phase so the verified RSP path
 *     is not disturbed; this header establishes the seam by aggregation now, so
 *     a future DAP front-end has one include and a documented boundary.
 */
#ifndef AOD_DEBUG_CORE_H
#define AOD_DEBUG_CORE_H

#include "updi.h"        /* execution control, reg/mem access, OCD, NVM        */
#include "elf_parser.h"  /* ELF symbols + flash translation + DWARF lookup     */
#include "fsm_mapper.h"  /* avrOS FSM task introspection snapshots             */
#include "monitor.h"     /* `monitor` command dispatch                         */

#endif /* AOD_DEBUG_CORE_H */
