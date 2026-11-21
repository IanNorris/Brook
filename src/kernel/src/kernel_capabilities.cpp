// App-facing contracts supplied by this kernel. The host extracts this
// section from the linked ELF, where optional features use the same compile
// definitions as the implementations they describe.
[[gnu::section(".brook_capabilities"), gnu::used]]
static constexpr char KernelCapabilities[] =
    "{"
    // Version 1 covers Chromium's user/mount/PID/net namespaces, seccomp-BPF,
    // and procfs fdinfo and namespace paths required by the launcher.
    "\"chromium-sandbox\":1"
#ifdef BROOK_BDW_VIDEO_ENGINE
    // Compiled support only. The target's hardware must still be admitted.
    ",\"broadwell-video-engine\":1"
#endif
    "}";
