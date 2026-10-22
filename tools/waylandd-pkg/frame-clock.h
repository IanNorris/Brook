#ifndef BROOK_FRAME_CLOCK_H
#define BROOK_FRAME_CLOCK_H
#include <stdint.h>
#include <limits.h>
enum { BROOK_FRAME_PERIOD_MS = 16 };
/* Keep the existing 16 ms cadence. Missed periods are skipped, never replayed.
 * Internal deadlines use 64-bit monotonic milliseconds; wl_callback's wire
 * timestamp may wrap independently at 32 bits without resetting this phase. */
struct brook_frame_clock { uint64_t next_ms; };
static int brook_frame_clock_start(struct brook_frame_clock *clock,uint64_t now) {
    if (now>UINT64_MAX-BROOK_FRAME_PERIOD_MS) return -1;
    clock->next_ms=now+BROOK_FRAME_PERIOD_MS;return BROOK_FRAME_PERIOD_MS;
}
static int brook_frame_clock_delay(struct brook_frame_clock *clock,uint64_t now) {
    if (now<clock->next_ms) {
        uint64_t remaining=clock->next_ms-now;
        // An earlier monotonic sample/regression is not an enormous timer arm.
        return remaining<=BROOK_FRAME_PERIOD_MS?(int)remaining:-1;
    }
    unsigned delay=BROOK_FRAME_PERIOD_MS-(unsigned)((now-clock->next_ms)%BROOK_FRAME_PERIOD_MS);
    if (now>UINT64_MAX-delay) return -1;
    clock->next_ms=now+delay;return (int)delay;
}
#endif
