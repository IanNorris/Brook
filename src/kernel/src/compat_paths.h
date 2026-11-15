#pragma once
#include <stddef.h>
#include <stdint.h>

// NixOS-style host-layout entry points (/bin/sh, /usr/bin/env).
//
// The root filesystem is a case-insensitive FAT initrd that cannot hold
// symlinks, so each distinct directory in the table receives its own small
// root-owned 0755 tmpfs at boot, populated with the listed symlinks. The
// mount point is created in the RAM initrd when missing; an existing
// non-empty directory (or non-directory) is never masked. Non-root callers
// cannot add entries (0755, uid 0); root may.
//
// The table walk is pure so host tests can drive it with a fake namespace.
namespace brook {

struct CompatPathLink {
    const char* directory; // absolute, no trailing slash, no "." / ".." parts
    const char* name;      // single path component
    const char* target;    // absolute symlink target
};

// Candidate for later: {"/lib64", "ld-linux-x86-64.so.2", <profile loader>}.
// Not enabled: provider-payload initrds already ship a real FAT /lib64 with
// the loader file, which this walk would (correctly) refuse to mask, and the
// interpreter must not dangle before /nix is mounted.
inline constexpr CompatPathLink kCompatPathLinks[] = {
    {"/bin", "sh", "/nix/profile/bin/sh"},
    {"/usr/bin", "env", "/nix/profile/bin/env"},
};
inline constexpr uint32_t kCompatPathLinkCount =
    sizeof(kCompatPathLinks) / sizeof(kCompatPathLinks[0]);
inline constexpr uint32_t kCompatPathMax = 128;

struct CompatPathOps {
    void* context;
    // 0 = existing directory, -2 = missing, -20 = exists but not a directory.
    int (*probe)(void* context, const char* path);
    int (*makeDirectory)(void* context, const char* path);
    // 0 = empty, 1 = has entries, negative = error.
    int (*entries)(void* context, const char* path);
    int (*mountTmpfs)(void* context, const char* path);
    int (*symlink)(void* context, const char* target, const char* path);
    // Optional. event is a static string; error is 0 for success events.
    void (*report)(void* context, const char* path, const char* event, int error);
};

struct CompatPathSummary {
    uint32_t mounted = 0, links = 0, skipped = 0, failed = 0;
};

namespace compat_paths_detail {
inline uint32_t Length(const char* s)
{
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}
inline bool Same(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
inline bool ValidComponent(const char* s, uint32_t n)
{
    if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
    for (uint32_t i = 0; i < n; ++i)
        if (s[i] == '/') return false;
    return true;
}
inline bool ValidDirectory(const char* s)
{
    const uint32_t n = Length(s);
    if (n < 2 || n >= kCompatPathMax || s[0] != '/' || s[n - 1] == '/') return false;
    for (uint32_t start = 1; start <= n;) {
        uint32_t end = start;
        while (end < n && s[end] != '/') ++end;
        if (!ValidComponent(s + start, end - start)) return false;
        start = end + 1;
    }
    return true;
}
inline void Report(const CompatPathOps& ops, const char* path, const char* event, int error)
{
    if (ops.report) ops.report(ops.context, path, event, error);
}
} // namespace compat_paths_detail

// Walk the table once. Each directory is handled at its first occurrence;
// later rows for the same directory add links to the same mount. A failure
// affects only that directory.
inline CompatPathSummary CompatPathsProvision(const CompatPathLink* links, uint32_t count,
                                              const CompatPathOps& ops)
{
    using namespace compat_paths_detail;
    CompatPathSummary summary{};
    for (uint32_t i = 0; i < count; ++i) {
        const char* directory = links[i].directory;
        bool seen = false;
        for (uint32_t j = 0; j < i && !seen; ++j) seen = Same(links[j].directory, directory);
        if (seen) continue;
        if (!ValidDirectory(directory)) {
            Report(ops, directory ? directory : "(null)", "invalid-directory", -22);
            ++summary.failed;
            continue;
        }
        // Create missing ancestors, then the mount point itself.
        char path[kCompatPathMax];
        const uint32_t length = Length(directory);
        int error = 0;
        bool usable = true;
        for (uint32_t end = 1; end <= length && !error && usable; ++end) {
            if (end != length && directory[end] != '/') continue;
            for (uint32_t k = 0; k < end; ++k) path[k] = directory[k];
            path[end] = 0;
            error = ops.probe(ops.context, path);
            if (error == -2) error = ops.makeDirectory(ops.context, path);
            else if (!error && end == length) {
                const int state = ops.entries(ops.context, path);
                if (state < 0) error = state;
                else if (state > 0) usable = false;
            }
        }
        if (error == -20 || !usable) {
            Report(ops, directory, error ? "not-a-directory" : "kept-existing", error);
            ++summary.skipped;
            continue;
        }
        if (!error) error = ops.mountTmpfs(ops.context, directory);
        if (error) {
            Report(ops, directory, "mount-failed", error);
            ++summary.failed;
            continue;
        }
        ++summary.mounted;
        uint32_t made = 0, wanted = 0;
        for (uint32_t j = i; j < count; ++j) {
            if (!Same(links[j].directory, directory)) continue;
            ++wanted;
            const char* name = links[j].name;
            const char* target = links[j].target;
            const uint32_t nameLength = Length(name);
            if (!ValidComponent(name, nameLength) || !target || target[0] != '/' ||
                length + 1 + nameLength >= kCompatPathMax) {
                Report(ops, directory, "invalid-link", -22);
                continue;
            }
            for (uint32_t k = 0; k < length; ++k) path[k] = directory[k];
            path[length] = '/';
            for (uint32_t k = 0; k <= nameLength; ++k) path[length + 1 + k] = name[k];
            error = ops.symlink(ops.context, target, path);
            if (error) Report(ops, path, "symlink-failed", error);
            else ++made;
        }
        summary.links += made;
        if (made != wanted) ++summary.failed;
        Report(ops, directory, "ready", 0);
    }
    return summary;
}

// Kernel adapter: provisions kCompatPathLinks in the initial mount namespace.
// Call once after the root initrd and tmpfs budget exist, before userspace.
CompatPathSummary CompatPathsProvisionBoot();

} // namespace brook
