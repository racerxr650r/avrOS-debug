/* src/monitor.h */
#ifndef AOD_MONITOR_H
#define AOD_MONITOR_H

#include "elf_parser.h"

int monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx,
                     const char *cmd);

#endif /* AOD_MONITOR_H */
