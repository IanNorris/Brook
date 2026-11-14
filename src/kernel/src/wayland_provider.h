#pragma once
// Which running processes already serve the desktop's Wayland socket.
//
// A desktop app launch used to run `run --service --once waylandd` before the
// app. `--once` matches the process basename only, so on a GPU desktop whose
// provider is /boot/INTLWL.ELF (a Vulkan-enabled waylandd build) the check
// missed it and started a second, CPU-only /nix/profile/bin/waylandd. That
// instance could not take wayland-0's lock, so it unlinked and re-bound the
// socket path itself. Every app launched afterwards connected to the CPU server,
// which composited (and scaled) its surfaces on the CPU. That was the Dell
// browser slowdown on 14 Nov 2026.

namespace brook {

// Basenames of executables that act as the desktop Wayland provider.
inline constexpr const char* WaylandProviderBaseNames[] = {"waylandd", "INTLWL.ELF"};

// True when `processName` (a basename optionally followed by "_NN", as stored
// in Process::name) is one of the provider basenames.
inline bool WaylandProviderProcessName(const char* processName)
{
    if (!processName) return false;
    for (const char* base : WaylandProviderBaseNames)
    {
        const char* a = processName;
        const char* b = base;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*b == '\0' && (*a == '\0' || *a == '_')) return true;
    }
    return false;
}

// `find(basename)` returns non-null when a live process has that basename
// (SchedulerFindProcessByBaseName in the kernel).
template<class Find> bool WaylandProviderRunning(Find find)
{
    for (const char* base : WaylandProviderBaseNames)
        if (find(base)) return true;
    return false;
}

} // namespace brook
