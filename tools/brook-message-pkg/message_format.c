#include "message_format.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static int append(char *out, size_t size, const char *format, ...) {
    size_t used = strlen(out);
    if (used >= size) return 0;
    va_list args; va_start(args, format);
    int length = vsnprintf(out + used, size - used, format, args);
    va_end(args);
    return length >= 0 && (size_t)length < size - used;
}
static void bytes(char *out, size_t size, uint64_t value) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double amount = (double)value; unsigned unit = 0;
    while (amount >= 1024 && unit < 4) { amount /= 1024; ++unit; }
    snprintf(out, size, "%.1f %s", amount, units[unit]);
}
int brook_system_message_format(const BrookSystemMessage *m, char *simple, size_t ss,
                                char *technical, size_t ts) {
    if (!simple || !technical || !ss || !ts) return 0;
    simple[0] = technical[0] = 0;
    if (!m || m->version != BROOK_SYSTEM_MESSAGE_VERSION || m->size != sizeof(*m) || (m->flags & ~255u) ||
        !memchr(m->app_name, 0, sizeof(m->app_name)) ||
        !memchr(m->executable, 0, sizeof(m->executable))) return 0;
    const char *app = m->app_name[0] ? m->app_name : "An application";
    const char *action = NULL;
    switch (m->code) {
    case BROOK_MESSAGE_APP_CLOSED_MEMORY:
        if (!append(simple, ss, "%s was closed because there wasn't enough memory for it to continue running.", app)) goto fail;
        action = "was terminated after system memory exhaustion"; break;
    case BROOK_MESSAGE_LAUNCH_MEMORY:
        if (!append(simple, ss, "%s couldn't open because there wasn't enough memory. Try closing another application.", app)) goto fail;
        action = "could not be launched because memory allocation failed"; break;
    case BROOK_MESSAGE_RECOVERY_STALLED:
        if (!append(simple, ss, "Brook is low on memory. %s is taking longer than expected to close.", app)) goto fail;
        action = "has not finished closing after a memory recovery request"; break;
    case BROOK_MESSAGE_NO_MEMORY_VICTIM:
        if (!append(simple, ss, "Brook is low on memory and couldn't safely close an application. Save your work and close an application if possible.")) goto fail;
        action = "has no eligible application to terminate for memory recovery"; app = "Brook"; break;
    case BROOK_MESSAGE_TERMINATION_REQUESTED:
        if (!append(simple, ss, "Brook is closing %s to make more memory available.", app)) goto fail;
        action = "was asked to terminate for memory recovery; cleanup is still pending"; break;
    default: goto fail;
    }
    if (!append(technical, ts, "%s%s%s%s %s.\nError code: %u; event: %" PRIu64 "; PID: %u; incarnation: %" PRIu64 ".",
        app, m->executable[0] ? " (" : "", m->executable,
        m->executable[0] ? ")" : "", action, m->code, m->sequence,
        m->victim_pid, m->victim_incarnation)) goto fail;
    if (m->requested_bytes) {
        char amount[48]; bytes(amount, sizeof(amount), m->requested_bytes);
        if (!append(technical, ts, "\nFailed request: %s (%" PRIu64 " bytes).", amount, m->requested_bytes)) goto fail;
    }
    if (m->flags & BROOK_MESSAGE_PHYSICAL_VALID) {
        if (!m->physical_total_bytes || m->physical_free_before_bytes > m->physical_total_bytes) goto fail;
        uint64_t free_bytes = m->physical_free_before_bytes;
        if (m->flags & BROOK_MESSAGE_AFTER_VALID) free_bytes = m->physical_free_after_bytes;
        if (free_bytes > m->physical_total_bytes) goto fail;
        char free_text[48], total_text[48]; bytes(free_text, sizeof(free_text), free_bytes); bytes(total_text, sizeof(total_text), m->physical_total_bytes);
        double used = 100. * (1. - (double)free_bytes / (double)m->physical_total_bytes);
        if (!append(technical, ts, "\nPhysical memory%s: %s free of %s (%.1f%% used).",
            m->flags & BROOK_MESSAGE_AFTER_VALID ? " after cleanup" : " at failure", free_text, total_text, used)) goto fail;
        if ((m->flags & BROOK_MESSAGE_AFTER_VALID) && free_bytes > m->physical_free_before_bytes) {
            char gain[48]; bytes(gain, sizeof(gain), free_bytes - m->physical_free_before_bytes);
            if (!append(technical, ts, "\nFree RAM increased by %s during recovery; concurrent allocations and reclaim also affect this value.", gain)) goto fail;
        }
    } else if (m->flags & BROOK_MESSAGE_AFTER_VALID) goto fail;
    if (m->flags & BROOK_MESSAGE_RELEASED_VALID) {
        char released[48]; bytes(released, sizeof(released), m->released_bytes);
        if (!append(technical, ts, "\nMeasured memory released by application cleanup: %s.", released)) goto fail;
    }
    if (!(m->flags & BROOK_MESSAGE_PAGING_STATE_VALID)) {
        if (m->flags & (BROOK_MESSAGE_PAGING_CONFIGURED | BROOK_MESSAGE_PAGING_USAGE_VALID |
                        BROOK_MESSAGE_PAGING_FULL | BROOK_MESSAGE_PAGING_EXPERIMENTAL)) goto fail;
        if (!append(technical, ts, "\nPaging status is unavailable.")) goto fail;
    } else if (!(m->flags & BROOK_MESSAGE_PAGING_CONFIGURED)) {
        if (m->flags & (BROOK_MESSAGE_PAGING_USAGE_VALID | BROOK_MESSAGE_PAGING_FULL | BROOK_MESSAGE_PAGING_EXPERIMENTAL)) goto fail;
        if (!append(technical, ts, "\nPaging backing is not configured.")) goto fail;
    } else if (m->flags & BROOK_MESSAGE_PAGING_USAGE_VALID) {
        if (!m->paging_capacity_bytes || m->paging_used_bytes > m->paging_capacity_bytes) goto fail;
        if ((m->flags & BROOK_MESSAGE_PAGING_FULL) && m->paging_used_bytes != m->paging_capacity_bytes) goto fail;
        char used[48], capacity[48]; bytes(used, sizeof(used), m->paging_used_bytes); bytes(capacity, sizeof(capacity), m->paging_capacity_bytes);
        if (!append(technical, ts, "\n%s backing: %s used of %s%s.",
            m->flags & BROOK_MESSAGE_PAGING_EXPERIMENTAL ? "Experimental paging" : "Paging",
            used, capacity, m->flags & BROOK_MESSAGE_PAGING_FULL ? " (full)" : "")) goto fail;
    } else if (!append(technical, ts, "\nPaging backing is configured; usage is unavailable.")) goto fail;
    return 1;
fail:
    simple[0] = technical[0] = 0; return 0;
}
