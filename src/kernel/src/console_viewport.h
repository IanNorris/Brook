#pragma once
#include <stdint.h>

namespace brook {
struct ConsoleInitialGeometry {
    uint32_t x, y, width, height;
};

inline uint32_t ConsoleVisibleHeight(int32_t clientY, uint32_t clientHeight,
                                     uint32_t desktopHeight) {
    const int64_t available = static_cast<int64_t>(desktopHeight) - clientY;
    if (available <= 0) return 0;
    return static_cast<uint64_t>(available) < clientHeight
        ? static_cast<uint32_t>(available) : clientHeight;
}

// Fit the console's client and its chrome above the taskbar. In particular,
// avoid rendering the newest lines below the visible area on small displays.
inline ConsoleInitialGeometry ConsoleFitInitial(uint32_t screenWidth, uint32_t desktopHeight,
                                                uint32_t desiredWidth, uint32_t desiredHeight,
                                                uint32_t border, uint32_t title) {
    const uint64_t chromeWidth = uint64_t{border} * 2;
    const uint64_t chromeHeight = uint64_t{border} * 2 + title;
    const uint32_t x = screenWidth > chromeWidth + 20 ? 20 : 0;
    const uint32_t y = desktopHeight > chromeHeight + 60 ? 60 : 0;
    const uint32_t availableWidth = screenWidth > uint64_t{x} + chromeWidth
        ? static_cast<uint32_t>(screenWidth - x - chromeWidth) : 0;
    const uint32_t availableHeight = desktopHeight > uint64_t{y} + chromeHeight
        ? static_cast<uint32_t>(desktopHeight - y - chromeHeight) : 0;
    return {x, y, desiredWidth < availableWidth ? desiredWidth : availableWidth,
                  desiredHeight < availableHeight ? desiredHeight : availableHeight};
}

// Absolute line positions keep the viewed history stable as new logs arrive.
struct ConsoleViewport {
    uint32_t end = 0;
    bool following = true;

    static uint32_t Oldest(uint32_t total, uint32_t retained) {
        return total > retained ? total - retained : 0;
    }
    static uint32_t MinimumEnd(uint32_t total, uint32_t retained, uint32_t visible) {
        const uint32_t oldest=Oldest(total,retained);
        const uint32_t available=total-oldest;
        return oldest+(available<visible?available:visible);
    }
    void Scroll(int lines, uint32_t total, uint32_t retained, uint32_t visible) {
        if(following || end>total)end=total;
        int64_t next=static_cast<int64_t>(end)-lines;
        const uint32_t minimum=MinimumEnd(total,retained,visible);
        if(next<minimum)next=minimum;
        if(next>total)next=total;
        end=static_cast<uint32_t>(next);
        following=end==total;
    }
    void Range(uint32_t total, uint32_t retained, uint32_t visible,
               uint32_t& start, uint32_t& count) {
        const uint32_t oldest=Oldest(total,retained);
        if(following || end>total)end=total;
        const uint32_t minimum=MinimumEnd(total,retained,visible);
        if(end<minimum)end=minimum;
        count=end-oldest;
        if(count>visible)count=visible;
        start=end-count;
    }
};
}
