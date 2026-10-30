#include "../../bootloader/src/mode_selection.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <utility>
using Mode=std::pair<unsigned,unsigned>;
static Mode choose(const std::array<Mode,5>& modes,unsigned rw,unsigned rh) {
    Mode best{};
    for(auto m:modes) if(brook::bootloader::PreferMode(m.first,m.second,best.first,best.second,rw,rh)) best=m;
    return best;
}
int main() {
    std::array<Mode,5> modes={{{640,480},{1280,720},{1920,1080},{2560,1440},{3840,2160}}};
    do {
        assert(choose(modes,0,0)==Mode(1920,1080));
        assert(choose(modes,1280,720)==Mode(1280,720));
        assert(choose(modes,1000,700)==Mode(640,480));
        assert(choose(modes,320,200)==Mode(640,480));
    } while(std::next_permutation(modes.begin(),modes.end()));
    modes={{{640,480},{1280,720},{1600,900},{2560,1440},{3840,2160}}};
    do { assert(choose(modes,0,0)==Mode(1280,720)); }
    while(std::next_permutation(modes.begin(),modes.end()));
    assert(!brook::bootloader::PreferMode(0,720,0,0,0,0));
}
