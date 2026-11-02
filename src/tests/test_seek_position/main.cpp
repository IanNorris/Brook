#include <cassert>
#include <cstdint>
#include <limits>
#include "seek_position.h"

int main() {
    uint64_t position=42;
    assert(brook::SeekPosition(0,4096,position) && position==4096);
    assert(brook::SeekPosition(position,-512,position) && position==3584);
    assert(brook::SeekPosition(1,-1,position) && position==0);
    position=42;
    assert(!brook::SeekPosition(0,-1,position) && position==42);
    assert(!brook::SeekPosition(1,std::numeric_limits<int64_t>::min(),position) && position==42);
    assert(!brook::SeekPosition(uint64_t(INT64_MAX),1,position) && position==42);
    assert(!brook::SeekPosition(uint64_t(INT64_MAX)+1,0,position) && position==42);
    assert(brook::SeekPosition(uint64_t(INT64_MAX),-1,position) && position==uint64_t(INT64_MAX)-1);
}
