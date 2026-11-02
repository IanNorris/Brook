#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "ext2_extended_time.h"
using namespace brook;

int main() {
    struct Case { int64_t seconds; uint32_t base, epoch; };
    const Case cases[] = {
        {-2147483648LL, 0x80000000u, 0}, {-1, 0xffffffffu, 0}, {0, 0, 0},
        {2147483647LL, 0x7fffffffu, 0}, {2147483648LL, 0x80000000u, 1},
        {4294967295LL, 0xffffffffu, 1}, {4294967296LL, 0, 1},
        {6442450943LL, 0x7fffffffu, 1}, {6442450944LL, 0x80000000u, 2},
        {8589934592LL, 0, 2}, {10737418239LL, 0x7fffffffu, 2},
        {10737418240LL, 0x80000000u, 3}, {12884901888LL, 0, 3},
        {15032385535LL, 0x7fffffffu, 3},
    };
    for (const auto& c : cases) for (uint32_t nanos : {0u, 1u, 999999999u}) {
        uint32_t base=0, extra=0;
        assert(Ext2ExtendedTimeEncode({c.seconds,nanos},base,extra));
        assert(base==c.base && (extra&3u)==c.epoch && (extra>>2)==nanos);
        Ext2ExtendedTime decoded{};
        assert(Ext2ExtendedTimeDecode(base,extra,decoded));
        assert(decoded.seconds==c.seconds && decoded.nanoseconds==nanos);
    }
    uint32_t base=123, extra=456;
    assert(!Ext2ExtendedTimeEncode({-2147483649LL,0},base,extra));
    assert(!Ext2ExtendedTimeEncode({15032385536LL,0},base,extra));
    assert(!Ext2ExtendedTimeEncode({0,1000000000u},base,extra));
    assert(base==123 && extra==456);
    Ext2ExtendedTime decoded{7,8};
    assert(!Ext2ExtendedTimeDecode(0,4000000000u,decoded));
    assert(decoded.seconds==7 && decoded.nanoseconds==8);
    return 0;
}
