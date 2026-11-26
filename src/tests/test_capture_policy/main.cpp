#include "../../kernel/src/wm_capture_protection.h"

#include <cassert>

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
    uint32_t pixels[12];
    for (auto& pixel : pixels) pixel = 0x996633;
    CaptureMaskPixels(pixels, 3, 3, 4, {-1, 1, 3, 4});
    CaptureMaskPixels(pixels, 3, 3, 4, {20, 0, 5, 1});
    for (unsigned i = 0; i < 12; ++i)
        assert(pixels[i] == (i >= 4 && i % 4 < 2 ? 0xff000000 : 0x996633));
}
