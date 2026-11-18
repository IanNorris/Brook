// Window-frame display list builder (compositor refactor stage 1.2). The
// primitives are the ones RenderWindowChrome used to draw directly, in the
// same order, so the CPU executor reproduces its pixels. Freestanding.
#include "chrome_frame.h"

namespace brook {

static void CaptionButtons(ChromeDisplayList& dl, const ChromeStyle& s, const ChromeFrameModel& m,
                           uint32_t bg, uint32_t fg, bool material, uint32_t hoverBg, uint32_t hoverInk,
                           const DesktopRect& clip = DesktopRect{}, unsigned radius = 0)
{
    for (int button = 0; button < 3; ++button)
    {
        const ChromePart* part = m.layout.Find(ChromeCaptionRole(button));
        if (!part) continue;
        const DesktopRect r = part->visual;
        const bool hover = part->hit.contains(m.mouseX, m.mouseY);
        if (material) dl.Fill(r.x, r.y, 1, r.h, ChromeMix(bg, 0xffffff, 20));
        if (hover) dl.CornerFill(r.x, r.y, r.w, r.h, button == 0 ? s.P().closeHover : hoverBg, clip, radius);
        if (r.w < static_cast<int>(s.M(14))) continue;
        const uint32_t ink = hover && button == 0 ? s.P().closeText : hover ? hoverInk : fg;
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
                dl.Fill(x, y + height - 1, width, s.M(1), ink);
                dl.Fill(x, y, s.M(1), height, ink);
                dl.Fill(x + width - 1, y, s.M(1), height, ink);
            };
            if (m.maximised)
            {
                box(cx - s.M(2), cy - s.M(5), s.M(8), s.M(8));
                dl.Fill(cx - s.M(5), cy - s.M(2), s.M(8), s.M(8), hover ? hoverBg : bg);
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
    const uint32_t edge = m.focused ? p.borderActive : p.titleInactive;
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
            CaptionButtons(dl, s, m, strip, idle | 0xff000000u, false, hoverBg, ink | 0xff000000u, block, radius);
            return;
        }
        dl.Fill(block.x, block.y, block.w, block.h, bg);
        CaptionButtons(dl, s, m, bg, fg, false, p.captionHover, p.text);
        return;
    }
    const int border = s.M(DesktopMetrics::BorderBase);
    const int x = m.layout.outer.x, y = m.layout.outer.y, ow = m.layout.outer.w, oh = m.layout.outer.h;
    dl.Fill(x, y, ow, border, edge);
    dl.Fill(x, y, border, oh, edge);
    dl.Fill(x + ow - border, y, border, oh, edge);
    dl.Fill(x, y + oh - border, ow, border, edge);
    const int tx = x + border, ty = y + border;
    const ChromeMaterialCache* cached = s.glass ? s.material : nullptr;
    const bool material = cached != nullptr;
    const int titleH = s.M(DesktopMetrics::TitleBase);
    if (material) dl.Rows(tx, ty, m.clientW, titleH, cached->rows[m.focused]);
    else dl.Fill(tx, ty, m.clientW, titleH, bg);
    // A restrained one-pixel edge replaces the old heavy double outline.
    if (border > 1) {
        dl.Fill(x + 1, y + 1, ow - 2, border - 1, bg);
        dl.Fill(x + 1, y + border, border - 1, oh - 2 * border, bg);
        dl.Fill(x + ow - border, y + border, border - 1, oh - 2 * border, bg);
        dl.Fill(x + 1, y + oh - border, ow - 2, border - 1, bg);
    }
    if (material) {
        dl.Fill(x, y, ow, 1, cached->rims[m.focused][0]);
        dl.Fill(x, y + 1, 1, oh - 2, cached->rims[m.focused][1]);
        dl.Fill(x + ow - 1, y + 1, 1, oh - 2, cached->rims[m.focused][1]);
        dl.Fill(x, y + oh - 1, ow, 1, cached->rims[m.focused][2]);
    }
    dl.Fill(tx, ty + titleH - 1, m.clientW, 1, material ? cached->rims[m.focused][2] : ChromeMix(bg, 0xffffff, 32));
    // Active identity stroke: keep the hit-test geometry and client opacity.
    if (m.focused) dl.Fill(tx + s.M(12), ty, m.clientW > static_cast<int>(s.M(184)) ? s.M(160) : m.clientW / 2,
                           s.M(4), p.accent);
    const ChromePart* minimise = m.layout.Find(ChromePartRole::Minimise);
    const int runLeft = minimise ? minimise->visual.x : x + ow;
    const int iconX = tx + s.M(10);
    const int textX = iconX + s.M(20) + s.M(8);
    if (runLeft - iconX >= static_cast<int>(s.M(24)))
        ChromeIcon(dl, s, iconX, ty + (static_cast<int>(s.M(DesktopMetrics::TitleBase)) - static_cast<int>(s.M(20))) / 2,
                   s.M(20), m.icon);
    const int textW = runLeft - textX - s.M(8);
    if (textW > 0)
        dl.Text(textX, ty + (static_cast<int>(s.M(DesktopMetrics::TitleBase)) - s.Font().lineHeight) / 2,
                m.title, fg, textW);
    CaptionButtons(dl, s, m, bg, fg, material, p.captionHover, p.text);
}

} // namespace brook
