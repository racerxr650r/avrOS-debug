/* src/dap.h — Debug Adapter Protocol (DAP) front-end (Phase 16+).
 *
 * A client-facing protocol front-end over the protocol-agnostic debug core
 * (debug_core.h), running **in parallel** to the GDB-RSP front-end in
 * gdb_rsp.c — selected at startup with `--dap` (`--rsp` is the default; see
 * main.c). It targets DAP-native editors, primarily VS Code, and is
 * automated-tested via headless Neovim + nvim-dap Lua scripts.
 *
 * Transport: `Content-Length: <n>\r\n\r\n<json>` JSON-RPC over the same TCP
 * listener and single select() event loop the RSP server uses.
 *
 * Phase 16 lands the foundation: this module, the `--dap`/`--rsp` mode
 * dispatch, the transport, and the lifecycle handshake
 * (initialize → launch/attach → configurationDone → disconnect). Execution
 * control + stop events (Phase 17), breakpoints + DWARF unwinding (Phase 18),
 * and variables/memory/evaluate (Phase 19) follow. See doc/SDP.md §8 and
 * doc/reference/dual-protocol-architecture.md.
 *
 * Layering: like gdb_rsp.c, this front-end translates wire framing to/from the
 * debug-core verbs and is the only layer that formats DAP replies; it never
 * reaches below the core (HLR-073).
 */
#ifndef AOD_DAP_H
#define AOD_DAP_H

#include <signal.h>     /* sig_atomic_t */
#include <stdbool.h>

#include "debug_core.h" /* ElfContext, AvrOsSymbolIndex, FsmContext + core API */

/* Serve the DAP front-end on the already-bound TCP listener `listen_fd` until
 * the client disconnects or `*quit` is set. The target UPDI link (`updi_fd`)
 * and the loaded ELF/DWARF (`elf`), avrOS symbol index (`idx`), and FSM
 * introspection context (`fsm`, may be NULL) are the debug-core handles the
 * DAP requests operate over. Returns 0 on clean shutdown, -1 on a fatal
 * setup error. */
int dap_serve(int listen_fd, int updi_fd,
              ElfContext *elf, const AvrOsSymbolIndex *idx,
              FsmContext *fsm, volatile sig_atomic_t *quit, bool log);

#endif /* AOD_DAP_H */
