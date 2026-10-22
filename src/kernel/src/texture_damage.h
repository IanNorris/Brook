#pragma once
#include <stdint.h>

namespace brook {
// Compositor-thread-owned pending upload bounds. Failed submission leaves the
// entire request dirty; callers never infer completion from a void callback.
struct TextureDamage {
    uint32_t first=~uint32_t(0),last=0;
    void Mark(uint32_t begin,uint32_t end) {
        if(begin>=end)return;
        if(begin<first)first=begin;
        if(end>last)last=end;
    }
    template<class Upload> bool Flush(bool& full,uint32_t width,uint32_t height,Upload upload) {
        if(!width || !height)return false;
        const uint32_t begin=full?0:first;
        const uint32_t end=full?height:(last<height?last:height);
        if(begin>=end)return true;
        if(!upload(0,begin,width,end-begin))return false;
        full=false;first=~uint32_t(0);last=0;return true;
    }
};
}
