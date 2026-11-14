#pragma once
#include <stdint.h>
#include "desktop_metrics.h"
#include "../../shared/brook_audio_control.h"

// Pure taskbar volume logic shared by the kernel window manager and host
// tests. The control value is the BROOK_SYS_AUDIO_CONTROL encoding (percent
// 0..100 plus BROOK_AUDIO_MUTED); nothing here touches the audio device.
namespace brook {

enum class TaskbarVolumeGlyph : uint8_t { Muted, Low, Medium, High };

inline TaskbarVolumeGlyph TaskbarVolumeGlyphFor(uint32_t control) {
    const unsigned v = brook_audio_volume(control);
    if ((control & BROOK_AUDIO_MUTED) || !v) return TaskbarVolumeGlyph::Muted;
    return v <= 33 ? TaskbarVolumeGlyph::Low : v <= 66 ? TaskbarVolumeGlyph::Medium : TaskbarVolumeGlyph::High;
}

inline uint32_t TaskbarVolumeClamp(int percent) {
    return percent < 0 ? 0u : percent > 100 ? 100u : static_cast<uint32_t>(percent);
}

// One wheel event moves one 5% grid step (the sign is used, not the
// magnitude, so high-resolution wheels cannot jump). Off-grid levels snap to
// the adjacent multiple of five. Raising the level unmutes; lowering keeps
// the mute state.
inline uint32_t TaskbarVolumeWheel(uint32_t control, int delta) {
    const int v = static_cast<int>(brook_audio_volume(control));
    uint32_t muted = control & BROOK_AUDIO_MUTED;
    int next = v;
    if (delta > 0) { next = (v / 5 + 1) * 5; muted = 0; }
    else if (delta < 0) next = ((v + 4) / 5 - 1) * 5;
    return TaskbarVolumeClamp(next) | muted;
}

inline uint32_t TaskbarVolumeToggleMute(uint32_t control) {
    return (control ^ BROOK_AUDIO_MUTED) & 0x17fu;
}

// Level for a pointer x over a horizontal track; the ends saturate. Setting
// a level from the slider unmutes, like most desktop mixers.
inline uint32_t TaskbarVolumeSliderValue(const DesktopRect& track, int x) {
    if (track.w <= 1) return 0;
    const int64_t offset = static_cast<int64_t>(x) - track.x;
    if (offset <= 0) return 0;
    if (offset >= track.w - 1) return 100;
    return static_cast<uint32_t>((offset * 100 + (track.w - 1) / 2) / (track.w - 1));
}

// Knob centre for a level, the inverse of TaskbarVolumeSliderValue.
inline int TaskbarVolumeSliderX(const DesktopRect& track, uint32_t percent) {
    if (track.w <= 1) return track.x;
    if (percent > 100) percent = 100;
    return track.x + static_cast<int>((static_cast<int64_t>(track.w - 1) * percent + 50) / 100);
}

struct TaskbarVolumePopupLayout {
    DesktopRect panel, mute, track, value;
};

// A panel above the taskbar centred on the icon and kept on screen:
// [mute button] [slider track] [percent text].
inline TaskbarVolumePopupLayout TaskbarVolumePopup(const DesktopMetrics& m, const DesktopRect& icon,
                                                   int taskbarY, int screenW, int valueTextW) {
    TaskbarVolumePopupLayout l{};
    const int pad = m.pixels(10), button = m.pixels(32);
    const int w = m.pixels(240), h = button + 2 * pad;
    const int margin = m.pixels(4);
    int x = icon.x + icon.w / 2 - w / 2;
    if (x + w > screenW - margin) x = screenW - margin - w;
    if (x < margin) x = margin;
    int y = taskbarY - h - m.pixels(2);
    if (y < 0) y = 0;
    l.panel = {x, y, w, h};
    l.mute = {x + pad, y + pad, button, button};
    const int valueW = valueTextW > 0 ? valueTextW : m.pixels(36);
    l.value = {x + w - pad - valueW, y + pad, valueW, button};
    const int trackX = l.mute.x + button + pad + m.pixels(6);
    const int trackRight = l.value.x - pad - m.pixels(6);
    l.track = {trackX, y + pad, trackRight > trackX ? trackRight - trackX : 0, button};
    return l;
}

enum class TaskbarVolumeHit : uint8_t { Outside, Panel, Mute, Slider };

// The slider accepts the whole row height, plus the knob overhang at the ends.
inline TaskbarVolumeHit TaskbarVolumePopupHit(const DesktopMetrics& m, const TaskbarVolumePopupLayout& l,
                                              int x, int y) {
    if (!l.panel.contains(x, y)) return TaskbarVolumeHit::Outside;
    if (l.mute.contains(x, y)) return TaskbarVolumeHit::Mute;
    const int slop = m.pixels(6);
    const DesktopRect grab{l.track.x - slop, l.track.y, l.track.w + 2 * slop, l.track.h};
    if (grab.contains(x, y)) return TaskbarVolumeHit::Slider;
    return TaskbarVolumeHit::Panel;
}

// "Volume: 45%", "Muted" or "No audio device"; out must hold 32 bytes.
inline void TaskbarVolumeTooltip(char* out, uint32_t control, bool available) {
    const char* text = !available ? "No audio device" : (control & BROOK_AUDIO_MUTED) ? "Muted" : nullptr;
    int n = 0;
    if (text) { while (text[n]) { out[n] = text[n]; ++n; } out[n] = 0; return; }
    const char* prefix = "Volume: ";
    while (prefix[n]) { out[n] = prefix[n]; ++n; }
    const unsigned v = brook_audio_volume(control);
    if (v >= 100) out[n++] = '1';
    if (v >= 10) out[n++] = static_cast<char>('0' + (v / 10) % 10);
    out[n++] = static_cast<char>('0' + v % 10);
    out[n++] = '%';
    out[n] = 0;
}

}
