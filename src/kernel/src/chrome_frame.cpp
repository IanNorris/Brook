// Window-frame display lists with explicit material and foreground alpha.
// Freestanding.
#include "chrome_frame.h"

namespace brook {

static void CaptionButtons(ChromeDisplayList& dl, const ChromeStyle& s, const ChromeFrameModel& m,
                           uint32_t fg, uint32_t hoverBg, uint32_t hoverInk)
{
    for (int button = 0; button < ChromeCaptionLimit; ++button)
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
        const int glyph = s.M(16);
        if (r.w < glyph || r.h < glyph) continue;
        const uint32_t ink = hover && button == 0 ? s.P().closeText : hover ? hoverInk : fg;
        // The 16 px line icons (DESIGN.md "Type and icons"), centred in the cell.
        const ChromeGlyph icon = button == 0 ? ChromeGlyph::Close : button == 2 ? ChromeGlyph::Minimise :
                                 button == 3 ? (m.layout.dockRight ? ChromeGlyph::DockRight : ChromeGlyph::DockLeft) :
                                 m.maximised ? ChromeGlyph::Restore : ChromeGlyph::Maximise;
        dl.Glyph(icon, r.x + (r.w - glyph) / 2, r.y + (r.h - glyph) / 2, ink);
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
            if (m.captionStrip && m.captionStrip->count >= static_cast<unsigned>(block.h))
                dl.CornerRows(block, radius, m.captionStrip->rgb);
            else
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
        // A 2 px light on the top rim: solid for 45% of 176 px, then fading.
        const int start = x + (m.maximised ? 0 : static_cast<int>(s.M(10)));
        const int limit = x + ow - (m.maximised ? 0 : static_cast<int>(s.M(10))) - start;
        const int width = limit < static_cast<int>(s.M(176)) ? limit : static_cast<int>(s.M(176));
        if (width > 0) {
            const int solid = width * 45 / 100;
            dl.Fill(start, y, solid, s.M(2), 0xff000000u | p.accent);
            dl.FadeX(start + solid, y, width - solid, s.M(2), 0xff000000u | p.accent);
        }
    }
    // The title ends at the leftmost caption button.
    const ChromePart* dock = m.layout.Find(ChromePartRole::Dock);
    const ChromePart* minimise = dock ? dock : m.layout.Find(ChromePartRole::Minimise);
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
        ChromeElidedText(dl, s, ChromeFont::Body, textX, ty + (titleH - s.Line()) / 2, m.title, fg, textW);
    CaptionButtons(dl, s, m, fg, s.MaterialColor(p.captionHover), p.text);
}

} // namespace brook
