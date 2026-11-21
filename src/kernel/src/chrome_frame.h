#pragma once
// Window-frame display list builder (compositor refactor stage 1.2): a
// window's part table and state in, the frame's primitives out. The kernel WM
// fills the model from its window (window.cpp); host tests fill it directly.
// Freestanding.
#include <stdint.h>
#include "../../shared/brook_caption_strip.h"
#include "chrome_layout.h"
#include "chrome_style.h"

namespace brook {

struct ChromeFrameModel {
    ChromeLayout layout;      // ChromeFrameLayout: visual cells and hit rects
    int clientW;
    bool focused, maximised;
    bool clientDecorated;     // CSD: only overlaid caption buttons, if any
    uint32_t captionStyle;    // CSD strip colour/radius (DesktopCaptionStyle*)
    const char* title;
    const uint32_t* icon;     // window icon (borrowed); null: missing glyph
    int32_t mouseX, mouseY;   // hover; a button is hovered anywhere in its hit rect
    const BrookCaptionStrip* captionStrip = nullptr;
};

void ChromeBuildFrame(const ChromeFrameModel& model, const ChromeStyle& style, ChromeDisplayList& out);

} // namespace brook
