// Capture policy shared by window admission and the capture exporter.
#pragma once

#include <stdint.h>

namespace brook {

inline char CaptureFold(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? char(ch + ('a' - 'A')) : ch;
}

// ASCII case-insensitive glob. '*' matches zero or more bytes and '?' one.
inline bool CaptureGlob(const char* pattern, uint32_t length, const char* value)
{
    uint32_t i = 0, star = length;
    const char* retry = value;
    while (*value) {
        if (i < length && pattern[i] == '*') {
            star = i++;
            retry = value;
        } else if (i < length && (pattern[i] == '?' || CaptureFold(pattern[i]) == CaptureFold(*value))) {
            ++i;
            ++value;
        } else if (star < length) {
            i = star + 1;
            value = ++retry;
        } else {
            return false;
        }
    }
    while (i < length && pattern[i] == '*') ++i;
    return i == length;
}

inline constexpr char DefaultCapturePolicy[] =
    "chromium*\t*(Incognito)\n"
    "google-chrome*\t*(Incognito)\n"
    "firefox*\t*Private Browsing*\n"
    "org.mozilla.firefox\t*Private Browsing*\n"
    "org.keepassxc.KeePassXC\t*\n"
    "org.gnome.World.Secrets\t*\n";

// One app ID glob and title glob per line, delimited by a tab. Both must match.
inline bool CapturePolicyMatches(const char* rules, const char* appId, const char* title)
{
    for (const char* line = rules; *line;) {
        const char* end = line;
        const char* tab = nullptr;
        while (*end && *end != '\n') {
            if (*end == '\t' && !tab) tab = end;
            ++end;
        }
        const char* limit = end > line && end[-1] == '\r' ? end - 1 : end;
        if (*line != '#' && tab && tab > line && tab + 1 < limit &&
            CaptureGlob(line, uint32_t(tab - line), appId) &&
            CaptureGlob(tab + 1, uint32_t(limit - tab - 1), title))
            return true;
        line = *end ? end + 1 : end;
    }
    return false;
}

// Called while the WM owns the window table. The latch makes the walk
// bounded even if a provider supplies a cycle. Creation identities survive
// slot reuse without accidentally binding an old popup to a new window.
template<class WindowType, unsigned Count>
bool LatchCaptureExclusion(WindowType (&windows)[Count], WindowType& window)
{
    if (window.captureExcluded) return false;
    window.captureExcluded = true;
    for (auto& child : windows)
        if (child.proc && child.captureParent == window.creationIdentity)
            LatchCaptureExclusion(windows, child);
    return true;
}

inline const char* CaptureShellLabel(bool excluded, const char* title)
{
    return excluded ? "Private window" : title;
}

struct CaptureMaskRect {
    int32_t x, y;
    uint32_t width, height;

    bool operator==(const CaptureMaskRect& other) const
    {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
};

inline constexpr uint32_t CaptureMaskCapacity = 32;

// Kernel-only metadata for the pixels actually published. A default record
// makes no claim about an image once any capture exclusion has been armed.
struct CapturePrivacy {
    uint64_t epoch = 0;
    uint32_t count = 0;
    bool known = false;
    CaptureMaskRect masks[CaptureMaskCapacity]{};

    bool permits(uint64_t currentEpoch) const
    {
        return !currentEpoch || (known && epoch == currentEpoch && count <= CaptureMaskCapacity);
    }

    bool matches(const CapturePrivacy& other) const
    {
        if (epoch != other.epoch || count != other.count || count > CaptureMaskCapacity ||
            (epoch && (!known || !other.known))) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (!(masks[i] == other.masks[i])) return false;
        return true;
    }
};

inline void CaptureMaskPixels(uint32_t* pixels, uint32_t width, uint32_t height,
                              uint32_t stride, const CaptureMaskRect& rect)
{
    const int64_t right = int64_t(rect.x) + rect.width;
    const int64_t bottom = int64_t(rect.y) + rect.height;
    const int64_t x0 = rect.x < 0 ? 0 : rect.x;
    const int64_t y0 = rect.y < 0 ? 0 : rect.y;
    const int64_t x1 = right < width ? right : width;
    const int64_t y1 = bottom < height ? bottom : height;
    for (int64_t y = y0; y < y1; ++y)
        for (int64_t x = x0; x < x1; ++x)
            pixels[uint64_t(y) * stride + x] = 0xff000000;
}

} // namespace brook
