#pragma once

// display_output.h — backend-neutral display outputs (multi-output D0).
//
// An output is one scanout head with a rectangle in desktop coordinates. A
// backend (the GOP/DisplayOps adapter today; Intel BDW flips and the NVIDIA
// window channels later) drives one or more outputs. The compositor sees
// only outputs, buffers and damage: never registers, channels or VRAM.
//
// The desktop is the union of the enabled outputs and starts at 0,0. Output
// 0 is the primary output. With one output the union is that output, so
// every reader that used the single framebuffer size gets the same answer.
//
// Everything in this header is pure and host-testable. The kernel registry
// instance and the GOP adapter live in display_output.cpp / compositor.cpp.

#include <stdint.h>

// BROOK_MULTI_OUTPUT=0 (the kernel CMake option, OFF on single-output
// machines) keeps the first output only and compiles the multi-output
// branches out. One output behaves the same either way.
#ifndef BROOK_MULTI_OUTPUT
#define BROOK_MULTI_OUTPUT 1
#endif

namespace brook {

inline constexpr bool OutputMultiSupported = BROOK_MULTI_OUTPUT != 0;
inline constexpr uint32_t OutputMax = 4;
inline constexpr uint32_t OutputBackendMax = 4;
// DRM fourcc 'XR24': the only format the compositor produces.
inline constexpr uint32_t OutputFormatXrgb8888 = 0x34325258u;
// Firmware framebuffers do not report a refresh rate.
inline constexpr uint32_t OutputNominalRefreshMhz = 60000;

struct OutputRect {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    constexpr bool empty() const { return !w || !h; }
    constexpr int64_t right() const { return int64_t(x) + w; }
    constexpr int64_t bottom() const { return int64_t(y) + h; }
    constexpr bool contains(int64_t px, int64_t py) const {
        return !empty() && px >= x && py >= y && px < right() && py < bottom();
    }
    constexpr bool operator==(const OutputRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct OutputInfo {
    uint8_t id;            // index in the enumerated layout (0 = primary)
    bool connected, enabled;
    int32_t x, y;          // desktop position
    uint32_t w, h;
    uint32_t stride;       // bytes per row of the scanout buffer
    uint32_t format;       // DRM fourcc
    uint32_t refreshMhz;
    uint32_t scalePercent;
    char name[16];         // "BROOK-1", ...
    char make[16], model[16];
    uint32_t mmW, mmH;     // physical size; 0 when unknown
    constexpr OutputRect rect() const { return {x, y, w, h}; }
};

// age: frames since this buffer was last presented (0 = new or unknown).
struct OutputBuffer {
    uint8_t* cpu;
    uint64_t gpuAddr;
    uint32_t stride;       // bytes
    uint32_t age;
};

// Half-open rectangle [x0,x1) x [y0,y1).
struct DamageRect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    constexpr bool empty() const { return x1 <= x0 || y1 <= y0; }
    constexpr bool operator==(const DamageRect& o) const {
        return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1;
    }
};

// A bounded damage list. Overflowing it merges the closest pair of rects
// (least added area, the new one included), so damage is never lost, only widened
// locally (damage on two outputs stays two rectangles). `full` asks the
// output for a complete repaint that may not trust any shadow of its
// previous contents (the rects then cover the whole area).
struct DamageRects {
    static constexpr uint32_t Max = 16;
    uint32_t count = 0;
    bool full = false;
    DamageRect rects[Max]{};

    constexpr bool empty() const { return count == 0; }
    bool Bounds(DamageRect* out) const {
        if (!count) return false;
        DamageRect b = rects[0];
        for (uint32_t i = 1; i < count; ++i) {
            const DamageRect& r = rects[i];
            if (r.x0 < b.x0) b.x0 = r.x0;
            if (r.y0 < b.y0) b.y0 = r.y0;
            if (r.x1 > b.x1) b.x1 = r.x1;
            if (r.y1 > b.y1) b.y1 = r.y1;
        }
        if (out) *out = b;
        return true;
    }
    void Add(const DamageRect& r) {
        if (r.empty()) return;
        for (uint32_t i = 0; i < count; ++i) {
            const DamageRect& c = rects[i];
            if (r.x0 >= c.x0 && r.y0 >= c.y0 && r.x1 <= c.x1 && r.y1 <= c.y1) return;
        }
        if (count == Max) {
            // Merge the pair (among the list and the new rect) whose union adds the least
            // area, so a far-away rect (another output) keeps its own slot.
            DamageRect all[Max + 1];
            for (uint32_t i = 0; i < count; ++i) all[i] = rects[i];
            all[count] = r;
            const uint32_t n = count + 1;
            uint32_t bi = 0, bj = 1;
            int64_t best = -1;
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t j = i + 1; j < n; ++j) {
                    const int64_t g = Area(Union(all[i], all[j])) - Area(all[i]) - Area(all[j]);
                    if (best < 0 || g < best) { best = g; bi = i; bj = j; }
                }
            all[bi] = Union(all[bi], all[bj]);
            count = 0;
            for (uint32_t i = 0; i < n; ++i) {
                if (i == bj) continue;
                const DamageRect& c = all[i];
                const DamageRect& m = all[bi];
                if (i != bi && c.x0 >= m.x0 && c.y0 >= m.y0 && c.x1 <= m.x1 && c.y1 <= m.y1) continue;
                rects[count++] = c;
            }
            return;
        }
        rects[count++] = r;
    }
    void AddAll(const DamageRects& o) {
        for (uint32_t i = 0; i < o.count; ++i) Add(o.rects[i]);
        full = full || o.full;
    }
    uint64_t Pixels() const {
        uint64_t n = 0;
        for (uint32_t i = 0; i < count; ++i) n += uint64_t(Area(rects[i]));
        return n;
    }
    static constexpr int64_t Area(const DamageRect& r) {
        return r.empty() ? 0 : int64_t(r.x1 - r.x0) * int64_t(r.y1 - r.y0);
    }
    static constexpr DamageRect Union(const DamageRect& a, const DamageRect& b) {
        return {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
    }
    // Damage of full-width rows [minY, maxY) of a `width` wide desktop: the
    // compositor's row span expressed as a rect list.
    static DamageRects Rows(uint32_t minY, uint32_t maxY, uint32_t width) {
        DamageRects d;
        d.rects[0] = {0, int32_t(minY), int32_t(width), int32_t(maxY)};
        d.count = d.rects[0].empty() ? 0 : 1;
        return d;
    }
    static DamageRects Full(uint32_t width, uint32_t height) {
        DamageRects d = Rows(0, height, width);
        d.full = d.count != 0;
        return d;
    }
};

// Clip desktop damage to one output and translate it into that output's
// local coordinates. `full` carries over when anything of the output is
// damaged.
inline DamageRects DamageClipToOutput(const DamageRects& damage, const OutputRect& out)
{
    DamageRects local;
    if (out.empty()) return local;
    for (uint32_t i = 0; i < damage.count; ++i) {
        const DamageRect& r = damage.rects[i];
        const int64_t x0 = r.x0 > out.x ? r.x0 : out.x, y0 = r.y0 > out.y ? r.y0 : out.y;
        const int64_t x1 = r.x1 < out.right() ? r.x1 : out.right();
        const int64_t y1 = r.y1 < out.bottom() ? r.y1 : out.bottom();
        if (x1 <= x0 || y1 <= y0) continue;
        local.Add({int32_t(x0 - out.x), int32_t(y0 - out.y), int32_t(x1 - out.x), int32_t(y1 - out.y)});
    }
    local.full = damage.full && local.count;
    return local;
}

enum class OutputFlipState : uint32_t { Idle = 0, Armed, Queued, Failed };

// One per backend; a backend may drive several outputs. Ids are the
// backend's own (0..Count-1). Optional ops may be nullptr.
struct OutputBackendOps {
    const char* name;
    uint32_t (*Count)();
    bool (*Info)(uint32_t id, OutputInfo* out);
    int  (*Enable)(uint32_t id);                    // may be asynchronous: >0 = pending
    int  (*Disable)(uint32_t id);
    bool (*AcquireBack)(uint32_t id, OutputBuffer* out); // a buffer not on screen
    // Present `damage` (output-local) of the compositor's desktop into
    // `buffer`. Returns >0 when pixels reached the display, 0 when nothing
    // changed, <0 on failure.
    int  (*Present)(uint32_t id, const OutputBuffer& buffer, const DamageRects& damage);
    uint32_t (*Poll)(uint32_t id);                  // an OutputFlipState
    uint64_t (*VblankCount)(uint32_t id);
    void (*SetPower)(uint32_t id, bool on);
    void (*Quiesce)();
    void (*PanicPresent)(uint32_t id, const uint8_t* src, uint32_t stride);
};

// A coherent snapshot of the outputs. Index i is the output id; 0 is the
// primary output.
struct OutputLayout {
    uint32_t count = 0;
    uint64_t generation = 0;
    OutputInfo outputs[OutputMax]{};
    const OutputBackendOps* backends[OutputMax]{};
    uint32_t backendIds[OutputMax]{};

    constexpr bool enabled(uint32_t i) const {
        return i < count && outputs[i].connected && outputs[i].enabled && !outputs[i].rect().empty();
    }
    constexpr OutputRect rect(uint32_t i) const { return enabled(i) ? outputs[i].rect() : OutputRect{}; }
    // More than one output: the multi-output paths apply.
    constexpr bool multi() const { return OutputMultiSupported && count > 1; }
};

// Backend list with a generation counter. Readers may enumerate without a
// lock; registration is rare and serialised by the caller.
class OutputRegistry {
public:
    constexpr OutputRegistry() = default;

    bool Register(const OutputBackendOps* ops) {
        if (!ops || !ops->Count || !ops->Info) return false;
        const uint32_t n = __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; i < n; ++i) if (backends_[i] == ops) return false;
        if (n >= OutputBackendMax) return false;
        backends_[n] = ops;
        __atomic_store_n(&count_, n + 1, __ATOMIC_RELEASE);
        NotifyChanged();
        return true;
    }
    bool Unregister(const OutputBackendOps* ops) {
        const uint32_t n = __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; i < n; ++i) {
            if (backends_[i] != ops) continue;
            for (uint32_t j = i; j + 1 < n; ++j) backends_[j] = backends_[j + 1];
            backends_[n - 1] = nullptr;
            __atomic_store_n(&count_, n - 1, __ATOMIC_RELEASE);
            NotifyChanged();
            return true;
        }
        return false;
    }
    // Any change of an output's geometry, mode, scale or presence.
    void NotifyChanged() { __atomic_add_fetch(&generation_, 1, __ATOMIC_ACQ_REL); }
    // Never 0, while the registry itself stays zero-initialised (.bss).
    uint64_t Generation() const { return __atomic_load_n(&generation_, __ATOMIC_ACQUIRE) + 1; }
    uint32_t BackendCount() const { return __atomic_load_n(&count_, __ATOMIC_ACQUIRE); }

    // Connected outputs in backend registration order, with ids renumbered
    // into the layout (the first backend's first output is the primary).
    void Enumerate(OutputLayout& out) const {
        out = OutputLayout{};
        out.generation = Generation();
        const uint32_t n = BackendCount(), limit = OutputMultiSupported ? OutputMax : 1;
        for (uint32_t b = 0; b < n && out.count < limit; ++b) {
            const OutputBackendOps* ops = backends_[b];
            const uint32_t outputs = ops->Count();
            for (uint32_t id = 0; id < outputs && out.count < limit; ++id) {
                OutputInfo info{};
                if (!ops->Info(id, &info) || !info.connected) continue;
                info.id = uint8_t(out.count);
                out.outputs[out.count] = info;
                out.backends[out.count] = ops;
                out.backendIds[out.count] = id;
                ++out.count;
            }
        }
    }

private:
    const OutputBackendOps* backends_[OutputBackendMax]{};
    uint32_t count_ = 0;
    uint64_t generation_ = 0;
};

// Bounding rectangle of the enabled outputs (empty when none).
inline OutputRect OutputUnionRect(const OutputLayout& layout)
{
    bool any = false;
    int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (uint32_t i = 0; i < layout.count; ++i) {
        if (!layout.enabled(i)) continue;
        const OutputRect r = layout.rect(i);
        if (!any) { x0 = r.x; y0 = r.y; x1 = r.right(); y1 = r.bottom(); any = true; continue; }
        if (r.x < x0) x0 = r.x;
        if (r.y < y0) y0 = r.y;
        if (r.right() > x1) x1 = r.right();
        if (r.bottom() > y1) y1 = r.bottom();
    }
    if (!any) return {};
    return {int32_t(x0), int32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
}

// The desktop starts at 0,0: its size reaches the union's right and bottom.
inline void OutputDesktopExtent(const OutputLayout& layout, uint32_t* w, uint32_t* h)
{
    const OutputRect u = OutputUnionRect(layout);
    const int64_t right = u.empty() ? 0 : u.right(), bottom = u.empty() ? 0 : u.bottom();
    if (w) *w = right > 0 ? uint32_t(right) : 0;
    if (h) *h = bottom > 0 ? uint32_t(bottom) : 0;
}

// The enabled output containing the point, or -1.
inline int OutputIndexForPoint(const OutputLayout& layout, int64_t px, int64_t py)
{
    for (uint32_t i = 0; i < layout.count; ++i)
        if (layout.enabled(i) && layout.rect(i).contains(px, py)) return int(i);
    return -1;
}

// The output a rectangle (a window) belongs to: the one containing its
// centre, else the one it overlaps most, else the primary output.
inline uint32_t OutputIndexForRect(const OutputLayout& layout, const OutputRect& r)
{
    // One output: every rect belongs to it (centre, overlap or fallback).
    if (!layout.multi()) return 0;
    const int found = OutputIndexForPoint(layout, int64_t(r.x) + r.w / 2, int64_t(r.y) + r.h / 2);
    if (found >= 0) return uint32_t(found);
    uint32_t best = 0;
    uint64_t bestArea = 0;
    for (uint32_t i = 0; i < layout.count; ++i) {
        if (!layout.enabled(i)) continue;
        const OutputRect o = layout.rect(i);
        const int64_t x0 = r.x > o.x ? r.x : o.x, y0 = r.y > o.y ? r.y : o.y;
        const int64_t x1 = r.right() < o.right() ? r.right() : o.right();
        const int64_t y1 = r.bottom() < o.bottom() ? r.bottom() : o.bottom();
        if (x1 <= x0 || y1 <= y0) continue;
        const uint64_t area = uint64_t(x1 - x0) * uint64_t(y1 - y0);
        if (area > bestArea) { bestArea = area; best = i; }
    }
    return best;
}

// The output's rectangle, or the primary's when `index` is not enabled.
inline OutputRect OutputRectOrPrimary(const OutputLayout& layout, uint32_t index)
{
    return layout.enabled(index) ? layout.rect(index) : layout.rect(0);
}

// Kernel registry (display_output.cpp). The compositor registers the GOP /
// DisplayOps adapter as the first backend, so its output is the primary.
bool OutputRegisterBackend(const OutputBackendOps* ops);
void OutputNotifyChanged();
uint64_t OutputGeneration();
void OutputEnumerate(OutputLayout& out);

} // namespace brook
