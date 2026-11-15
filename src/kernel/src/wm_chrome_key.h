#pragma once
#include <stdint.h>
// Content key for one window's rendered chrome (title bar and border).
// Two captures with equal keys produce identical pixels, so a renderer that
// already holds the pixels for a key can keep them instead of having the
// kernel re-render and copy the whole outer surface every frame.
// Window position is deliberately excluded: chrome is rendered at (0,0)
// and placed by the returned metadata.

namespace brook {

struct WmChromeKeyInput {
    const char* title; uint32_t titleCapacity;
    const char* appId; uint32_t appIdCapacity;
    uint64_t windowIdentity;      // creation identity: also fixes proc/exe icon fallback
    uint64_t metricsIdentity;     // per-window decoration geometry table
    uint32_t outerWidth,outerHeight;
    uint32_t clientWidth,clientHeight;
    int32_t clientOffsetX,clientOffsetY;
    uint32_t state;
    bool focused,visible,noChrome;
    uint64_t captionOverlay;      // 0, or bit 63 | height << 32 | top inset << 16 | right inset
    int32_t hoverButton;          // -1 none, else caption button index
    uint32_t theme,appearance,scale;
    uint64_t settingsEpoch,launcherEpoch;
};

struct WmChromeKeyHasher {
    uint64_t h=1469598103934665603ull;
    void byte(uint8_t b){h^=b;h*=1099511628211ull;}
    void u64(uint64_t v){for(int i=0;i<8;++i)byte(uint8_t(v>>(i*8)));}
    void text(const char* s,uint32_t capacity){
        uint32_t n=0;
        if(s)while(n<capacity && s[n]){byte(uint8_t(s[n]));++n;}
        u64(n); // length terminator separates adjacent strings
    }
};

inline uint64_t WmChromeKey(const WmChromeKeyInput& in) {
    WmChromeKeyHasher k;
    k.text(in.title,in.titleCapacity);
    k.text(in.appId,in.appIdCapacity);
    k.u64(in.windowIdentity);k.u64(in.metricsIdentity);
    k.u64((uint64_t(in.outerWidth)<<32)|in.outerHeight);
    k.u64((uint64_t(in.clientWidth)<<32)|in.clientHeight);
    k.u64((uint64_t(uint32_t(in.clientOffsetX))<<32)|uint32_t(in.clientOffsetY));
    k.u64(in.state);
    k.byte(uint8_t(in.focused)|uint8_t(in.visible)<<1|uint8_t(in.noChrome)<<2);
    k.u64(in.captionOverlay);
    k.u64(uint64_t(int64_t(in.hoverButton)));
    k.u64((uint64_t(in.theme)<<32)|in.appearance);k.u64(in.scale);
    k.u64(in.settingsEpoch);k.u64(in.launcherEpoch);
    return k.h|1; // never zero: zero means "no key held"
}

// Counters for the renderer's per-frame chrome requests.
struct WmChromeCaptureStats {
    uint64_t keeps;        // unchanged chrome kept by the renderer: no render, no copy
    uint64_t keepMisses;   // keep refused because the chrome changed (renderer recaptures)
    uint64_t captures;     // full renders copied to the renderer
    uint64_t bytesCopied;  // bytes copied by captures
    uint64_t bytesKept;    // bytes the keeps avoided copying
};

// Defined in syscall.cpp; updated with relaxed atomics.
extern WmChromeCaptureStats g_chromeCaptureStats;

} // namespace brook
