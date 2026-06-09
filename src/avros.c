/* src/avros.c — avrOS runtime-table introspection (see avros.h).
 *
 * The single home for reading the avrOS event/queue descriptor tables off the
 * target.  Both front-ends — the GDB-RSP `monitor avros …` formatter and the
 * DAP avrosdb eventList/queueList custom requests — call these readers and
 * apply their own formatting, so the table-decode logic is not duplicated and
 * the DAP front-end does not depend on the RSP one.
 */
#include "avros.h"
#include "updi.h"   /* updi_mem_read, flash_to_updi */

#include <stddef.h>
#include <stdio.h>
#include <ctype.h>

/* avrOS runtime descriptor sizes (must match sys/event.c, sys/queue.c). */
#define EVNT_DESCR_SIZE 4u
#define QUE_DESCR_SIZE  10u

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Follow a target pointer and read a NUL-terminated string (non-printable
 * bytes rendered as '?'); returns the length, or -1 on a UPDI read error. */
static int read_target_string(int updi_fd, uint32_t addr, char *out, size_t out_max)
{
    if (out_max == 0) return -1;
    uint32_t base = flash_to_updi(addr);
    size_t   n    = 0;
    while (n + 1u < out_max) {
        uint8_t b;
        if (updi_mem_read(updi_fd, base + (uint32_t)n, &b, 1u) < 0) return -1;
        if (b == 0u) break;
        if (!isprint((unsigned char)b)) b = '?';
        out[n++] = (char)b;
    }
    out[n] = '\0';
    return (int)n;
}

int avros_read_events(const AvrOsSymbolIndex *idx, int updi_fd,
                      AvrosEvent *out, int max)
{
    if (idx == NULL || out == NULL || max <= 0) return -1;
    if (idx->event_count == 0u) return 0;

    static uint8_t descr[EVNT_DESCR_SIZE * 256u];
    size_t total = (size_t)idx->event_count * EVNT_DESCR_SIZE;
    if (updi_mem_read(updi_fd, flash_to_updi(idx->event_table_addr),
                      descr, total) < 0)
        return -1;

    int n = 0;
    for (uint8_t i = 0; i < idx->event_count && n < max; ++i) {
        const uint8_t *d = &descr[i * EVNT_DESCR_SIZE];
        uint16_t name_ptr   = le16(&d[0]);
        uint16_t status_ptr = le16(&d[2]);

        if (name_ptr == 0u) {
            snprintf(out[n].name, sizeof out[n].name, "<null>");
        } else if (read_target_string(updi_fd, name_ptr + idx->flash_lma_off,
                                      out[n].name, sizeof out[n].name) < 0) {
            return -1;
        }

        out[n].status     = 0u;
        out[n].has_status = false;
        if (status_ptr != 0u) {
            uint8_t s = 0u;
            if (updi_mem_read(updi_fd, status_ptr, &s, 1u) < 0) return -1;
            out[n].status     = s;
            out[n].has_status = true;
        }
        n++;
    }
    return n;
}

int avros_read_queues(const AvrOsSymbolIndex *idx, int updi_fd,
                      AvrosQueue *out, int max)
{
    if (idx == NULL || out == NULL || max <= 0) return -1;
    if (idx->queue_count == 0u) return 0;

    static uint8_t descr[QUE_DESCR_SIZE * 256u];
    size_t total = (size_t)idx->queue_count * QUE_DESCR_SIZE;
    if (updi_mem_read(updi_fd, flash_to_updi(idx->queue_table_addr),
                      descr, total) < 0)
        return -1;

    int n = 0;
    for (uint8_t i = 0; i < idx->queue_count && n < max; ++i) {
        /* layout: queue*(2) buffer*(2) event*(2) capacity(2) sizeOfElement(2) */
        const uint8_t *d = &descr[i * QUE_DESCR_SIZE];
        out[n].capacity  = le16(&d[6]);
        out[n].elem_size = le16(&d[8]);
        n++;
    }
    return n;
}
