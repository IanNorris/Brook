#pragma once
#include <stdint.h>
#include "desktop_metrics.h"

// Launcher power controls: footer geometry, hit testing and the two-step
// confirmation. Pure logic shared by the kernel window manager and host tests.
//
// The footer sits below a separator, apart from the app grid, with the power
// buttons right-aligned (Clearwater Next places Restart and Shut down at the
// right end of the launcher footer). A press on a power button only arms a
// confirmation; it never acts. The prompt replaces the cluster: Cancel covers
// the whole former cluster, so a double-click or a repeated tap at the first
// position dismisses. The distinct confirm button sits left of the cluster,
// never overlapping either original button, and ignores presses during a short
// settle period after arming.
//
// Keyboard: Enter on a focused power button arms the prompt, exactly like a
// click. In the prompt, focus starts on Cancel; Tab or the arrows move it,
// Enter activates the focused button and Escape dismisses.
namespace brook {

enum class LauncherPowerTarget : uint8_t {
    None,           // not in the footer
    Footer,         // footer band or separator gap, no button: inert
    Shutdown,       // idle buttons
    Restart,
    CancelShutdown, // a desktop shutdown is in progress
    Confirm,        // prompt buttons
    Dismiss,
    ForceApps,
};

enum class LauncherPowerAction : uint8_t { None, Shutdown, Restart };
enum class LauncherPowerCommand : uint8_t { None, Shutdown, Restart, CancelShutdown, ForceApps };
enum class LauncherPowerKey : uint8_t { Enter, Escape, Next, Previous };

struct LauncherPowerMetrics {
    int footerH;    // button band height
    int pad;        // panel inner padding
    int gap;        // between adjacent buttons
    int separation; // between the confirm button and the cluster
    int lead;       // separator gap above the footer band
    int buttonW;    // each idle power button (and half the Cancel)
    int confirmW;   // "Restart now" / "Shut down now"
};

struct LauncherPowerLayout {
    DesktopRect footer;   // full band, including the inert left area
    DesktopRect zone;     // band plus the separator gap above it
    DesktopRect separator;
    DesktopRect restart, shutdown, cluster;
    DesktopRect confirm, dismiss;
};

// Panel width needed for the prompt without clipping the confirm button.
inline int LauncherPowerMinimumWidth(const LauncherPowerMetrics& m) {
    return 2 * m.pad + m.confirmW + m.separation + 2 * m.buttonW + m.gap;
}

// Height the footer adds below the app grid (lead gap and band).
inline int LauncherPowerFooterHeight(const LauncherPowerMetrics& m) { return m.lead + m.footerH; }

inline LauncherPowerLayout LauncherPowerFooter(const DesktopRect& panel, const LauncherPowerMetrics& m) {
    LauncherPowerLayout l{};
    const int w = panel.w - 2 * m.pad;
    if (w <= 0 || m.footerH <= 0 || panel.h < 2 * m.pad + m.footerH) return l;
    const int y = panel.y + panel.h - m.pad - m.footerH;
    l.footer = {panel.x + m.pad, y, w, m.footerH};
    l.zone = {l.footer.x, y - m.lead, w, m.footerH + m.lead};
    l.separator = {l.footer.x, y - (m.lead + 1) / 2, w, 1};
    const int right = l.footer.x + w;
    // Narrow panels shrink the buttons; the cluster never leaves the footer.
    int bw = m.buttonW;
    if (2 * bw + m.gap > w) bw = (w - m.gap) / 2;
    if (bw <= 0) return l;
    l.shutdown = {right - bw, y, bw, m.footerH};
    l.restart = {l.shutdown.x - m.gap - bw, y, bw, m.footerH};
    l.cluster = {l.restart.x, y, right - l.restart.x, m.footerH};
    l.dismiss = l.cluster;
    // The confirm button stays clear of the cluster; if the panel is too
    // narrow it is clipped (and on an absurd panel omitted), never moved in.
    const int confirmRight = l.cluster.x - m.separation;
    int confirmX = confirmRight - m.confirmW;
    if (confirmX < l.footer.x) confirmX = l.footer.x;
    if (confirmRight - confirmX > 0) l.confirm = {confirmX, y, confirmRight - confirmX, m.footerH};
    return l;
}

// What a point hits, given the footer state.
inline LauncherPowerTarget LauncherPowerHit(const LauncherPowerLayout& l, bool prompt,
                                            bool shutdownActive, int x, int y) {
    if (!l.zone.contains(x, y)) return LauncherPowerTarget::None;
    if (shutdownActive) return l.cluster.contains(x, y) ? LauncherPowerTarget::CancelShutdown
                                                        : LauncherPowerTarget::Footer;
    if (prompt) {
        if (l.confirm.contains(x, y)) return LauncherPowerTarget::Confirm;
        if (l.dismiss.contains(x, y)) return LauncherPowerTarget::Dismiss;
        return LauncherPowerTarget::Footer;
    }
    if (l.shutdown.contains(x, y)) return LauncherPowerTarget::Shutdown;
    if (l.restart.contains(x, y)) return LauncherPowerTarget::Restart;
    return LauncherPowerTarget::Footer;
}

class LauncherPowerGuard {
public:
    // A confirm press this soon after arming is ignored (not a dismissal).
    static constexpr uint64_t SettleMs = 400;

    LauncherPowerAction pending() const { return m_pending; }
    bool prompt() const { return m_pending != LauncherPowerAction::None; }
    bool confirmFocused() const { return m_confirmFocus; }
    void Reset() { m_pending = LauncherPowerAction::None; m_confirmFocus = false; }

    // A pointer press, or Enter on a focused button, on a footer target.
    LauncherPowerCommand Activate(LauncherPowerTarget target, uint64_t nowMs) {
        switch (target) {
        case LauncherPowerTarget::Shutdown:
        case LauncherPowerTarget::Restart:
            if (!prompt()) {
                m_pending = target == LauncherPowerTarget::Restart ? LauncherPowerAction::Restart
                                                                   : LauncherPowerAction::Shutdown;
                m_armedAt = nowMs;
                m_confirmFocus = false;
            }
            return LauncherPowerCommand::None;
        case LauncherPowerTarget::Confirm: {
            if (!prompt() || nowMs - m_armedAt < SettleMs) return LauncherPowerCommand::None;
            const auto action = m_pending;
            Reset();
            return action == LauncherPowerAction::Restart ? LauncherPowerCommand::Restart
                                                          : LauncherPowerCommand::Shutdown;
        }
        case LauncherPowerTarget::Dismiss:
            Reset();
            return LauncherPowerCommand::None;
        case LauncherPowerTarget::CancelShutdown:
            Reset();
            return LauncherPowerCommand::CancelShutdown;
        case LauncherPowerTarget::ForceApps:
            Reset();
            return LauncherPowerCommand::ForceApps;
        case LauncherPowerTarget::None:
        case LauncherPowerTarget::Footer:
            break;
        }
        return LauncherPowerCommand::None;
    }

    // Keys while the prompt is shown. Returns the command to run, if any.
    LauncherPowerCommand Key(LauncherPowerKey key, uint64_t nowMs) {
        if (!prompt()) return LauncherPowerCommand::None;
        switch (key) {
        case LauncherPowerKey::Escape: Reset(); break;
        case LauncherPowerKey::Next:
        case LauncherPowerKey::Previous: m_confirmFocus = !m_confirmFocus; break;
        case LauncherPowerKey::Enter:
            return Activate(m_confirmFocus ? LauncherPowerTarget::Confirm : LauncherPowerTarget::Dismiss, nowMs);
        }
        return LauncherPowerCommand::None;
    }

private:
    LauncherPowerAction m_pending = LauncherPowerAction::None;
    uint64_t m_armedAt = 0;
    bool m_confirmFocus = false;
};

} // namespace brook
