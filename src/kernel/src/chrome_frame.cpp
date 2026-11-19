// Window-frame display lists with explicit material and foreground alpha.
// Freestanding.
#include "chrome_frame.h"

namespace brook {

static void CaptionButtons(ChromeDisplayList& dl, const ChromeStyle& s, const ChromeFrameModel& m,
                           uint32_t fg, uint32_t hoverBg, uint32_t hoverInk)
{
    for (int button = 0; button < 3; ++button)
    {
        const ChromePart* part = m.layout.Find(ChromeCaptionRole(button));
        if (!part) continue;
        const DesktopRect r = part->visual;
        const bool hover = part->hit.contains(m.mouseX, m.mouseY);
        if (hover) {
            const int width = r.w < static_cast<int>(s.M(36)) ? r.w : s.M(36);
            const int height = r.h < static_cast<int>(s.M(26)) ? r.h : s.M(26);
            dl.MaterialRect({r.x + (r.w - width) / 2, r.y + (r.h - height) / 2, width, height},
                            button == 0 ? 0xff000000u | s.P().closeHover : hoverBg, s.M(6));
        }
        if (r.w < static_cast<int>(s.M(14))) continue;
        const uint32_t ink = 0xff000000u | (hover && button == 0 ? s.P().closeText : hover ? hoverInk : fg);
        const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (button == 0)
        {
            const int k = static_cast<int>(s.M(4));
            dl.Squares(cx - k, cy - k, s.M(1), s.M(1), 1, 1, 2 * k + 1, ink);
            dl.Squares(cx - k, cy + k, s.M(1), s.M(1), 1, -1, 2 * k + 1, ink);
        }
        else if (button == 1)
        {
            auto box = [&](int x, int y, int width, int height) {
                dl.Fill(x, y, width, s.M(1), ink);
                dl.Fill(x, y + height - s.M(1), width, s.M(1), ink);
                dl.Fill(x, y, s.M(1), height, ink);
                dl.Fill(x + width - s.M(1), y, s.M(1), height, ink);
            };
            if (m.maximised)
            {
                // Draw only the visible edges of the rear square.
                dl.Fill(cx - s.M(2), cy - s.M(5), s.M(8), s.M(1), ink);
                dl.Fill(cx + s.M(5), cy - s.M(5), s.M(1), s.M(8), ink);
                dl.Fill(cx - s.M(2), cy - s.M(5), s.M(1), s.M(3), ink);
                dl.Fill(cx + s.M(3), cy + s.M(2), s.M(3), s.M(1), ink);
                box(cx - s.M(5), cy - s.M(2), s.M(8), s.M(8));
            }
            else box(cx - s.M(5), cy - s.M(4), s.M(11), s.M(9));
        }
        else dl.Fill(cx - s.M(5), cy + s.M(3), s.M(11), s.M(1), ink);
    }
}

void ChromeBuildFrame(const ChromeFrameModel& m, const ChromeStyle& s, ChromeDisplayList& dl)
{
    const BrookThemePalette& p = s.P();
    const uint32_t edge = 0xff000000u | (m.focused ? p.borderActive : p.borderInactive);
    const uint32_t bg = s.MaterialColor(m.focused ? p.titleActive : p.titleInactive);
    const uint32_t fg = m.focused ? p.text : p.mutedText;
    if (m.clientDecorated)
    {
        // Overlaid captions: only the button block, in the native title
        // material, over the client's own title area. The client keeps the
        // rest of its top edge (Chromium's tab strip).
        const ChromePart* close = m.layout.Find(ChromePartRole::Close);
        const ChromePart* minimise = m.layout.Find(ChromePartRole::Minimise);
        if (!close || !minimise) return;
        const DesktopRect block{minimise->visual.x, close->visual.y,
                                close->visual.x + close->visual.w - minimise->visual.x, close->visual.h};
        if (block.w <= 0 || block.h <= 0) return;
        if (m.captionStyle & DesktopCaptionStyleValid)
        {
            // Matched to the client's strip (as Chromium's own buttons, or
            // macOS/Windows controls over an app-drawn title area): the strip
            // colour hides the client's buttons underneath, glyphs contrast
            // with it, hover is a light tint and close hover stays red.
            const uint32_t strip = 0xff000000u | DesktopCaptionStyleRgb(m.captionStyle);
            const uint32_t ink = DesktopCaptionInk(strip & 0xffffff);
            const uint32_t idle = m.focused ? ink : ChromeMix(ink, strip, 100);
            const uint32_t hoverBg = 0xff000000u | ChromeMix(strip, ink, 28);
            const unsigned radius = m.maximised ? 0 : DesktopCaptionStyleRadius(m.captionStyle);
            dl.CornerFill(block.x, block.y, block.w, block.h, strip, block, radius);
            CaptionButtons(dl, s, m, idle, hoverBg, ink);
            return;
        }
        dl.Fill(block.x, block.y, block.w, block.h, 0xff000000u | bg);
        CaptionButtons(dl, s, m, fg, 0xff000000u | p.captionHover, p.text);
        return;
    }
    const int border = s.M(DesktopMetrics::BorderBase);
    const int x = m.layout.outer.x, y = m.layout.outer.y, ow = m.layout.outer.w, oh = m.layout.outer.h;
    const ChromeMaterialCache* cached = s.glass ? s.material : nullptr;
    const int tx = x + border, ty = y + border;
    const int titleH = s.M(DesktopMetrics::TitleBase);
    const uint32_t flat = 0xff000000u | p.titleInactive;
    const uint32_t title = cached ? cached->rows[m.focused][0] : bg;
    const uint32_t top = cached ? cached->rims[m.focused][0] : edge;
    const uint32_t divider = cached ? (title & 0xff000000u) | p.titleDivider : 0xff000000u | p.titleDivider;
    dl.Fill(x, y, ow, border, title);
    dl.Fill(x, y + border, border, oh - border, flat);
    dl.Fill(x + ow - border, y + border, border, oh - border, flat);
    dl.Fill(x, y + oh - border, ow, border, flat);
    if (cached && cached->height >= static_cast<unsigned>(titleH))
        dl.Rows(x, ty, ow, titleH, cached->rows[m.focused]);
    else dl.Fill(x, ty, ow, titleH, bg);
    dl.Fill(x, y, ow, 1, top);
    dl.Fill(x, y + 1, 1, oh - 2, edge);
    dl.Fill(x + ow - 1, y + 1, 1, oh - 2, edge);
    dl.Fill(x, y + oh - 1, ow, 1, edge);
    if (cached) {
        const uint32_t highlight = (title & 0xff000000u) | ChromeMix(title, 0xffffff, p.innerHighlight);
        dl.Fill(x + 1, y + 1, ow - 2, 1, highlight);
    }
    dl.Fill(tx, ty + titleH - 1, m.clientW, 1, divider);
    if (m.focused) {
        const int start = x + (m.maximised ? 0 : static_cast<int>(s.M(8)));
        const int width = x + ow - start;
        dl.FadeX(start, y, width < static_cast<int>(s.M(176)) ? width : s.M(176), s.M(2), 0xff000000u | p.accent);
    }
    const ChromePart* minimise = m.layout.Find(ChromePartRole::Minimise);
    const int runLeft = minimise ? minimise->visual.x : x + ow;
    const int iconX = tx + s.M(10);
    const int icon = s.M(16);
    const int textX = iconX + icon + s.M(8);
    if (runLeft - iconX >= icon + static_cast<int>(s.M(4))) {
        const int iconY = ty + (titleH - icon) / 2;
        if (!m.focused && m.icon)
            dl.Icon(iconX, iconY, icon, m.icon, 176);
        else ChromeIcon(dl, s, iconX, iconY, icon, m.icon);
    }
    const int textW = runLeft - textX - s.M(8);
    if (textW > 0)
        dl.Text(textX, ty + (static_cast<int>(s.M(DesktopMetrics::TitleBase)) - s.Font().lineHeight) / 2,
                m.title, fg, textW);
    CaptionButtons(dl, s, m, fg, s.MaterialColor(p.captionHover), p.text);
}

} // namespace brook
