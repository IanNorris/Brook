// Host test for the PMM quarantine drain batching policy (pmm_drain_policy.h).
#include <cstdio>
#include <cstdlib>
#include "memory/pmm_drain_policy.h"
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort();} } while(0)
using namespace brook;
int main() {
    const uint64_t total = 2u << 20;           // 8 GiB of 4 KiB pages
    const uint64_t plenty = total / 2;
    // Nothing pending: never drain again without napping.
    CHECK(!PmmDrainContinue(0, plenty, total));
    CHECK(!PmmDrainContinue(0, 0, total));
    // Trickle frees with ample memory: defer to the next nap (one barrier later).
    CHECK(!PmmDrainContinue(1, plenty, total));
    CHECK(!PmmDrainContinue(PmmDrainMinBatch - 1, plenty, total));
    // A batch that amortises the barrier drains immediately (BRO-206 rate-matching).
    CHECK(PmmDrainContinue(PmmDrainMinBatch, plenty, total));
    CHECK(PmmDrainContinue(100000, plenty, total));
    // Memory tight: drain any pending frames immediately.
    CHECK(PmmDrainContinue(1, total / 32 - 1, total));
    CHECK(!PmmDrainContinue(1, total / 32, total));
    // Near the producer throttle (low watermark total/16): drain immediately.
    const uint64_t small = 4096;               // tiny machine: total/32 < min batch
    CHECK(PmmDrainContinue(small / 32, small, small));
    // Unknown size (early boot): eager.
    CHECK(PmmDrainContinue(1, 0, 0));
    std::puts("pmm drain policy: deferral, batch, pressure and watermark PASS");
}
