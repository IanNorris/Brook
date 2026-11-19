#include <assert.h>
#include "latency-stats.h"

int main(void) {
    struct latency_hist hist = {0};
    assert(!latency_record(&hist, 0, 10));
    assert(!latency_record(&hist, 10, 9));
    assert(!hist.count);
    assert(latency_record(&hist, 1, 1));
    assert(!latency_record(&hist, 1, 1));
    assert(hist.buckets[0] == 2 && hist.count == 2 && hist.max_us == 0);
    for (unsigned i = 0; i + 1 < LATENCY_BUCKETS; ++i) {
        hist = (struct latency_hist){0};
        const uint64_t bound = latency_bounds_ms[i] * 1000ull;
        assert(latency_record(&hist, 1, bound));
        assert(latency_record(&hist, 1, bound + 1));
        assert(latency_record(&hist, 1, bound + 2));
        assert(hist.buckets[i] == 2 && hist.buckets[i+1] == 1);
        assert(hist.count == 3 && hist.max_us == bound + 1);
    }
    assert(latency_record(&hist, 1, UINT64_MAX));
    assert(hist.max_us == UINT64_MAX - 1 && hist.buckets[LATENCY_BUCKETS-1] == 2);

    uint64_t last = 0, max = 0;
    latency_axis_sent(1, &last, &max);
    latency_axis_sent(1001, &last, &max);
    assert(max == 1000);
    latency_axis_sent(500001, &last, &max);
    assert(max == 1000);
    latency_axis_sent(500001 + LATENCY_AXIS_BURST_US, &last, &max);
    assert(max == LATENCY_AXIS_BURST_US);
    return 0;
}
