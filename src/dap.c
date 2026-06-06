/* src/dap.c — Debug Adapter Protocol (DAP) front-end.
 *
 * Phase 16 scaffold. The `--dap` mode dispatch (main.c), this module, and the
 * front-end/core seam are in place; the transport (Content-Length JSON-RPC),
 * the lifecycle handshake, and the request handlers are implemented
 * incrementally across Phases 16–19 (see doc/SDP.md §8). Until the transport
 * lands, `dap_serve()` reports that the front-end is under construction and
 * returns a fatal status so the operator falls back to `--rsp` (the default).
 */
#include "dap.h"

#include <stdio.h>

int dap_serve(int listen_fd, int updi_fd,
              ElfContext *elf, const AvrOsSymbolIndex *idx,
              FsmContext *fsm, volatile sig_atomic_t *quit, bool log)
{
    (void)listen_fd; (void)updi_fd; (void)elf; (void)idx;
    (void)fsm; (void)quit; (void)log;

    fprintf(stderr,
            "avrOSdb: --dap selected, but the DAP front-end is still under "
            "construction (Phase 16). Use --rsp (the default) for now.\n");
    return -1;
}
