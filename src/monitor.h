/* src/monitor.h */
#ifndef AOD_MONITOR_H
#define AOD_MONITOR_H

#include "elf_parser.h"
#include "gdb_rsp.h"

/* Legacy entry point: handles only the `avros <verb>` sub-commands and
 * uses the supplied (rsp_fd, updi_fd, idx) directly.  Kept stable so the
 * pre-HLR-055 unit tests still link.  Return values:
 *    0   verb handled, caller should reply OK
 *   -1   verb recognised but underlying UPDI / I/O failed
 *   -2   verb not recognised or argument error (usage hint already sent)
 */
int monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx,
                     const char *cmd);

/* HLR-055: extended entry that handles the avarice-compatible top-level
 * verbs (`reset`, `halt`, `go`, `erase`, `chip-erase`, `version`,
 * `bp-mode`, `help`) and delegates to monitor_dispatch() for the
 * `avros ` prefix.  Additional return code:
 *   -3   verb handled, but caller must NOT send a trailing OK (a stop-
 *        reply or E-packet was already emitted by the verb).
 */
int monitor_dispatch_ex(int rsp_fd, RspContext *ctx, const char *cmd);

#endif /* AOD_MONITOR_H */
