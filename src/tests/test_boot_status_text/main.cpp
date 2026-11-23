#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <tuple>
#include "boot_status_text.h"
#include "boot_splash.h"
using namespace brook;
int main()
{
    assert(!strcmp(BootStatusText("Network"), "Starting network"));
    assert(!strcmp(BootStatusText("Desktop services"), "Starting desktop"));
    assert(!strcmp(BootStatusText("Storage"), "Finding storage"));
    assert(!BootStatusText("Networks") && !BootStatusText("") && !BootStatusText(nullptr));
    for (const char* s : {"Kernel","Memory","ACPI","SMP","Filesystem","Storage","Modules","Display","Network",
                          "Syscalls","Scheduler","Desktop services","Desktop assets","Ready"}) {
        const char* t = BootStatusText(s);
        assert(t && *t && !strchr(t, '%'));
    }
    // The overlay rectangle is exactly the bar DrawBootSplash paints, at the
    // real 4K mode and a small one where the logo is halved.
    const uint32_t logo[4 * 4] = {};
    for (auto [w, h, lw, lh] : {std::tuple<uint32_t,uint32_t,uint32_t,uint32_t>{3840, 2160, 256, 256},
                                {200, 150, 256, 64}}) {
        std::vector<uint32_t> fb(size_t(w) * h, 0x123456);
        std::vector<uint32_t> big(size_t(lw) * lh, 0);
        DrawBootSplash(fb.data(), w, h, w, big.data(), lw, lh, 50);
        const auto bar = BootSplashBarRect(w, h, lw, lh);
        assert(bar.width && bar.x + bar.width <= w && bar.y + 8 <= h);
        assert(fb[size_t(bar.y) * w + bar.x] == 0x4c80c0);                       // filled part
        assert(fb[size_t(bar.y + 7) * w + bar.x + bar.width - 1] == 0x293342);   // empty part
        assert(bar.y == 0 || fb[size_t(bar.y - 1) * w + bar.x] != 0x4c80c0);     // starts here
    }
    (void)logo;
    std::puts("boot status text: PASS");
}
