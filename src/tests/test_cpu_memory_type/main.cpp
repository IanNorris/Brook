#include "../../kernel/src/cpu_memory_type.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) std::abort(); } while(0)
int main() {
    using namespace brook;
    constexpr uint64_t reset=0x0007040600070406ULL,wc=0x0007040100070406ULL;
    uint64_t target=0;
    CHECK(CpuPatWithWriteCombining(reset,target)&&target==wc);
    CHECK(CpuPatWithWriteCombining(wc,target)&&target==wc);
    for (unsigned slot=0;slot<8;++slot) {
        target=42;
        CHECK(!CpuPatWithWriteCombining(reset^(uint64_t{0xff}<<(slot*8)),target));
        CHECK(target==42);
    }
    std::puts("CPU_PAT_PASS preserves existing types, reserves unused slot4, rejects unfamiliar firmware");
}
