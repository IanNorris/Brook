#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "../../kernel/src/cpu_execute_disable.h"

int main()
{
    constexpr uint64_t nxe = uint64_t{1} << 11;
    for (bool extended : {false, true}) for (bool supported : {false, true})
    for (bool enabled : {false, true}) for (bool accepts : {false, true}) {
        uint64_t efer = 0x501 | (enabled ? nxe : 0), before = 0, after = 0;
        const auto original = efer;
        unsigned reads = 0, writes = 0, queries = 0;
        auto cpuid = [&](uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
            ++queries; b = c = d = 0;
            if (leaf == 0x80000000u) a = extended ? 0x80000001u : 0x80000000u;
            else { assert(extended && leaf == 0x80000001u); a = 0; d = supported ? (1u << 20) : 0; }
        };
        const bool ready = brook::CpuEnableExecuteDisable(cpuid,
            [&] { ++reads; return efer; },
            [&](uint64_t value) { ++writes; assert(value == (original | nxe)); if (accepts) efer = value; },
            before, after);
        assert(queries == (extended ? 2u : 1u));
        if (!extended || !supported) {
            assert(!ready && !reads && !writes && efer == original);
        } else {
            assert(reads == 2 && writes == (enabled ? 0u : 1u));
            assert(ready == (enabled || accepts));
            assert(before == original && after == efer);
            assert((efer & ~nxe) == (original & ~nxe));
        }
    }
    return 0;
}
