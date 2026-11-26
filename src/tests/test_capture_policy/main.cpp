#include "../../kernel/src/wm_capture_protection.h"
#include "../../kernel/src/desktop_capture_access.h"

#include <cassert>
#include <cstring>
#include <string>

int main()
{
    using namespace brook;
    assert(CapturePolicyMatches(DefaultCapturePolicy, "chromium-browser", "New Tab (Incognito)"));
    assert(CapturePolicyMatches(DefaultCapturePolicy, "org.mozilla.firefox", "Example — Private Browsing"));
    assert(CapturePolicyMatches(DefaultCapturePolicy, "org.keepassxc.KeePassXC", ""));
    assert(!CapturePolicyMatches(DefaultCapturePolicy, "firefox", "Example — Mozilla Firefox"));
    assert(!CapturePolicyMatches(DefaultCapturePolicy, "brook.editor", "New Tab (Incognito)"));
    assert(CapturePolicyMatches("# comment\r\norg.Test.??\t*SeCrEt*\r\n", "org.test.ab", "a secret"));
    assert(!CapturePolicyMatches("org.Test.??\t*Secret*\n", "org.test.abc", "a secret"));
    assert(!CapturePolicyMatches("org.test\t\n\t*\norg.test\n", "org.test", ""));
    const std::string longTitle = std::string(100, 'x') + " — Mozilla Firefox Private Browsing";
    assert(CapturePolicyMatches(DefaultCapturePolicy, "firefox", longTitle.c_str()));
    assert(!CapturePolicyMatches(DefaultCapturePolicy, "firefox", longTitle.substr(0, 63).c_str()));
    assert(!std::strcmp(CaptureShellLabel(true, longTitle.c_str()), "Private window"));
    assert(CaptureShellLabel(false, longTitle.c_str()) == longTitle.c_str());
    struct Popup { bool proc; uint64_t creationIdentity, captureParent; bool captureExcluded; };
    Popup windows[] = {{true, 10, 0, false}, {true, 13, 10, false},
                       {true, 14, 13, false}, {true, 19, 0, false},
                       {true, 20, 9, false}};
    assert(LatchCaptureExclusion(windows, windows[0]));
    assert(windows[0].captureExcluded && windows[1].captureExcluded && windows[2].captureExcluded);
    assert(!windows[3].captureExcluded && !windows[4].captureExcluded);
    assert(!LatchCaptureExclusion(windows, windows[0]));
    assert(DesktopLayerContentOperation(BROOK_DESKTOP_LAYERS_QUERY));
    assert(DesktopLayerContentOperation(BROOK_DESKTOP_LAYERS_CAPTURE));
    assert(DesktopLayerContentOperation(BROOK_DESKTOP_CHROME_CAPTURE));
    assert(DesktopLayerContentOperation(BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_SPANS));
    assert(!DesktopLayerContentOperation(BROOK_DESKTOP_FOCUS_QUERY));
    assert(DesktopLayerContentAllowed(true, true, true, true));
    assert(!DesktopLayerContentAllowed(false, true, true, true));
    assert(!DesktopLayerContentAllowed(true, true, false, true));
    assert(!DesktopLayerContentAllowed(true, false, true, true));
    assert(!DesktopLayerContentAllowed(true, true, true, false));
    uint32_t pixels[12];
    for (auto& pixel : pixels) pixel = 0x996633;
    CaptureMaskPixels(pixels, 3, 3, 4, {-1, 1, 3, 4});
    CaptureMaskPixels(pixels, 3, 3, 4, {20, 0, 5, 1});
    for (unsigned i = 0; i < 12; ++i)
        assert(pixels[i] == (i >= 4 && i % 4 < 2 ? 0xff000000 : 0x996633));
}
