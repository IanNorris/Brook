// The desktop launch path must recognise the GPU desktop's INTLWL.ELF as the
// Wayland provider, so it does not start a second CPU waylandd.
#include "wayland_provider.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    using brook::WaylandProviderProcessName;
    assert(WaylandProviderProcessName("waylandd"));
    assert(WaylandProviderProcessName("waylandd_5"));
    assert(WaylandProviderProcessName("INTLWL.ELF"));
    assert(WaylandProviderProcessName("INTLWL.ELF_0"));
    assert(!WaylandProviderProcessName("waylanddx"));
    assert(!WaylandProviderProcessName("INTLWL"));
    assert(!WaylandProviderProcessName("chromium"));
    assert(!WaylandProviderProcessName(nullptr));

    const char* running = nullptr;
    auto find = [&](const char* base) { return running && std::strcmp(base, running) == 0; };
    assert(!brook::WaylandProviderRunning(find));
    running = "INTLWL.ELF";
    assert(brook::WaylandProviderRunning(find));
    running = "waylandd";
    assert(brook::WaylandProviderRunning(find));
    std::puts("test_wayland_provider: ok");
    return 0;
}
