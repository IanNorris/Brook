#ifndef BROOK_LATENCY_STATS_H
#define BROOK_LATENCY_STATS_H

#include <stdint.h>

static const uint32_t latency_bounds_ms[] = {4, 8, 16, 33, 50, 100, 250, 500, 1000};
enum { LATENCY_BUCKETS = sizeof(latency_bounds_ms) / sizeof(latency_bounds_ms[0]) + 1 };
enum { LATENCY_AXIS_BURST_US = 250000 };

struct latency_hist {
    uint64_t buckets[LATENCY_BUCKETS];
    uint64_t count, max_us;
};

/* Return true when the caller should capture a new worst-event context. */
static inline int latency_record(struct latency_hist *hist, uint64_t start_us, uint64_t end_us) {
    if (!start_us || end_us < start_us) return 0;
    const uint64_t elapsed_us = end_us - start_us;
    unsigned bucket = 0;
    while (bucket + 1 < LATENCY_BUCKETS && elapsed_us > 1000ull * latency_bounds_ms[bucket])
        ++bucket;
    ++hist->buckets[bucket];
    const int worst = !hist->count++ || elapsed_us > hist->max_us;
    if (worst) hist->max_us = elapsed_us;
    return worst;
}

static inline void latency_axis_sent(uint64_t now, uint64_t *last, uint64_t *max_gap) {
    if (*last && now >= *last) {
        const uint64_t gap = now - *last;
        if (gap <= LATENCY_AXIS_BURST_US && gap > *max_gap) *max_gap = gap;
    }
    *last = now;
}

#endif
