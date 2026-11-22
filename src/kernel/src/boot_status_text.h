#pragma once
// Boot splash status line: the user-facing text for each kernel boot stage
// label passed to BootLogoProgress. Pure and host-tested; no percentages.
namespace brook {
inline const char* BootStatusText(const char* stage)
{
    struct Row { const char* stage; const char* text; };
    static constexpr Row rows[] = {
        {"Kernel", "Starting kernel"}, {"Memory", "Starting kernel"}, {"ACPI", "Starting kernel"},
        {"SMP", "Starting processors"}, {"Filesystem", "Mounting boot image"},
        {"Storage", "Finding storage"}, {"Modules", "Starting drivers"}, {"Display", "Starting drivers"},
        {"Network", "Starting network"}, {"Syscalls", "Starting system services"},
        {"Scheduler", "Starting system services"}, {"Desktop services", "Starting desktop"},
        {"Desktop assets", "Loading desktop"}, {"Ready", "Starting desktop"},
    };
    if (!stage) return nullptr;
    for (const auto& row : rows) {
        const char* a = stage; const char* b = row.stage;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) return row.text;
    }
    return nullptr; // unknown labels keep the previous line
}
}
