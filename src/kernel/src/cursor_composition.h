#pragma once
#include <stdint.h>

namespace brook {
constexpr uint32_t CursorCapacity = 64;
struct CursorImage {
    uint32_t width = 12, height = 16;
    int32_t hotX = 0, hotY = 0;
    uint32_t pixels[CursorCapacity * CursorCapacity]{};
};

struct CursorUploadState {
    CursorImage accepted{};
    bool valid=false;
    bool Matches(const CursorImage& frame) const {
        if(!valid || accepted.width!=frame.width || accepted.height!=frame.height)return false;
        for(uint32_t i=0;i<CursorCapacity*CursorCapacity;++i)
            if(accepted.pixels[i]!=frame.pixels[i])return false;
        return true;
    }
    template<class Upload> bool Update(const CursorImage& frame,bool force,Upload upload) {
        if(!force && Matches(frame))return true;
        if(!upload(frame.width,frame.height))return false;
        accepted=frame;valid=true;return true;
    }
};

// Compose into a separate frame: client cursor pixels/hotspot survive busy
// animation and are restored verbatim when the last pending launch resolves.
inline void ComposeCursor(const CursorImage& source, CursorImage& frame,
                          bool busy, uint64_t nowMs, uint32_t accent)
{
    frame = source;
    if (!busy) return;
    constexpr int dx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
    constexpr int dy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
    const uint32_t cx = source.width + 4 < CursorCapacity - 6 ? source.width + 4 : CursorCapacity - 6;
    const uint32_t cy = source.height / 2 + 4 < CursorCapacity - 6 ? source.height / 2 + 4 : CursorCapacity - 6;
    if (frame.width < cx + 6) frame.width = cx + 6;
    if (frame.height < cy + 6) frame.height = cy + 6;
    const unsigned phase = (nowMs / 100) % 8;
    for (unsigned dot = 0; dot < 8; ++dot) {
        const uint32_t x = static_cast<uint32_t>(static_cast<int>(cx) + dx[dot]);
        const uint32_t y = static_cast<uint32_t>(static_cast<int>(cy) + dy[dot]);
        const uint32_t color = dot == phase ? (accent | 0xff000000u) : 0xff53677bu;
        for (uint32_t yy = 0; yy < 2; ++yy)
            for (uint32_t xx = 0; xx < 2; ++xx)
                frame.pixels[(y + yy) * CursorCapacity + x + xx] = color;
    }
}
}
