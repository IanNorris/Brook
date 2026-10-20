#include <cstdio>
#include "../../kernel/src/posix_timer.cpp"

namespace {
uint64_t monotonicNs = 9750000000ULL;
uint64_t realtimeNs = 1700000000250000000ULL;
bool pvclock = true;
unsigned failures = 0;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "POSIX CLOCK FAIL line %d: %s\n", __LINE__, #x); \
    ++failures; } } while (0)
}

namespace brook {
volatile uint64_t g_lapicTickCount = 0;
bool KvmClockAvailable() { return pvclock; }
uint64_t KvmClockReadNs() { return monotonicNs; }
uint64_t RtcNowNs() { return realtimeNs; }
uint64_t RtcNow() { return realtimeNs / 1000000000ULL; }
}

int main() {
    using namespace brook;
    // Deliberately unrelated fractional epochs. Exercise the production POSIX
    // clock selector and expiry code, not a second implementation of either.
    CHECK(Now(0) == realtimeNs);
    CHECK(Now(1) == monotonicNs);
    Timer timer{};
    timer.armed = true;
    timer.notify = 1; // SIGEV_NONE: observe expiry without signal scaffolding.
    timer.deadline = realtimeNs + 100000000ULL;
    Advance(timer, Now(0));
    CHECK(timer.armed);
    // Cross the pvclock second boundary while realtime stays in its second.
    monotonicNs += 250000000ULL;
    realtimeNs += 250000000ULL;
    CHECK(Now(0) == realtimeNs);
    Advance(timer, Now(0));
    CHECK(!timer.armed);
    // A realtime boundary must not borrow the monotonic fractional second.
    realtimeNs = 1700000000999999999ULL;
    CHECK(Now(0) == realtimeNs);
    ++realtimeNs;
    CHECK(Now(0) == realtimeNs);
    // Non-KVM realtime must retain the RTC driver's own elapsed source.
    pvclock = false;
    g_lapicTickCount = 123;
    CHECK(Now(0) == realtimeNs);
    CHECK(Now(1) == 123000000ULL);
    std::printf("POSIX timer clock: %u failures\n", failures);
    return failures ? 1 : 0;
}
