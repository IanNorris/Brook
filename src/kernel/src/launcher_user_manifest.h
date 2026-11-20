#pragma once

#include <stdint.h>
#include "desktop_identity.h"
#include "launcher_model.h"

namespace brook {

inline constexpr char UserLauncherManifestPath[] = "/data/home/.local/share/brook/user-apps.idx";
inline constexpr char UserLauncherIconDirectory[] = "/data/home/.local/share/brook/icons";
inline constexpr char UserLauncherProfileBin[] = "/nix/profile/bin/";
inline constexpr char UserLauncherRunPrefix[] = "--uid 1000 ";
static_assert(DesktopUid == 1000);
inline constexpr uint32_t UserLauncherManifestLimit = 64 * 1024;
inline constexpr uint32_t UserLauncherRecordLimit = 1024;
inline constexpr uint32_t UserLauncherEntryLimit = 64;
inline constexpr uint32_t UserLauncherCommandCapacity = 128;

struct UserLauncherEntry {
    char title[48];
    char command[UserLauncherCommandCapacity];
    char executable[64];
    char icon[128];
    char categories[256];
    char appId[LauncherLimits::KeyLength];
};

inline bool UserLauncherManifestMode(uint16_t mode, uint32_t uid)
{
    return uid == DesktopUid && (mode == 0100600 || mode == 0100644);
}

inline bool UserLauncherAsciiWord(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

// The shell treats a leading # as a comment and double quotes as token
// syntax. Keep plain arguments only; reject internal run flags anywhere.
inline bool UserLauncherSafeArgument(const char* text, uint32_t length)
{
    if (!length || *text == '#') return false;
    for (uint32_t i = 0; i < length; ++i) {
        const auto c = static_cast<uint8_t>(text[i]);
        if (c < 0x21 || c > 0x7e || c == '"' || c == '\'' || c == '\\') return false;
    }
    const char* runFlags[] = {"--uid", "--gid", "--as", "--strace", "--strace-errors",
                              "--service", "--once", "--quiet", "--console"};
    for (const char* flag : runFlags) {
        uint32_t i = 0;
        while (i < length && flag[i] && text[i] == flag[i]) ++i;
        if (!flag[i] && (i == length || text[i] == '=')) return false;
    }
    return true;
}

inline bool UserLauncherBuildCommand(const char* text, uint32_t length, UserLauncherEntry& out)
{
    constexpr uint32_t pathLength = sizeof(UserLauncherProfileBin) - 1;
    constexpr uint32_t optionLength = sizeof(UserLauncherRunPrefix) - 1;
    uint32_t end = 0;
    while (end < length && text[end] != ' ') ++end;
    if (!end) return false;
    uint32_t start = 0;
    if (*text == '/') {
        if (end <= pathLength) return false;
        for (; start < pathLength; ++start)
            if (text[start] != UserLauncherProfileBin[start]) return false;
    }
    const uint32_t nameLength = end - start;
    if (text[start] == '-' || nameLength >= sizeof(out.executable) ||
        (text[start] == '.' &&
         (nameLength == 1 || (nameLength == 2 && text[start + 1] == '.'))))
        return false;
    for (uint32_t i = start; i < end; ++i)
        if (!UserLauncherAsciiWord(text[i]) && text[i] != '+') return false;

    if (optionLength + pathLength + length - start >= sizeof(out.command)) return false;
    // ParseLine allows 32 tokens including "run", "--uid", the UID and the
    // executable. Reject rather than letting ParseLine discard a suffix.
    uint32_t arguments = 0;
    for (uint32_t i = end; i < length;) {
        while (i < length && text[i] == ' ') ++i;
        if (i == length) break;
        const uint32_t first = i;
        while (i < length && text[i] != ' ') ++i;
        if (++arguments > 28 || !UserLauncherSafeArgument(text + first, i - first)) return false;
    }

    for (uint32_t i = 0; i < nameLength; ++i) out.executable[i] = text[start + i];
    out.executable[nameLength] = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < optionLength; ++i) out.command[n++] = UserLauncherRunPrefix[i];
    for (uint32_t i = 0; i < pathLength; ++i) out.command[n++] = UserLauncherProfileBin[i];
    for (uint32_t i = start; i < length; ++i) out.command[n++] = text[i];
    out.command[n] = 0;
    return true;
}

inline bool UserLauncherValidTitle(const char* text, uint32_t length)
{
    if (!length || text[0] == ' ' || text[length - 1] == ' ') return false;
    // Titles are UTF-8. Command arguments, IDs, categories and icon names
    // use separate, narrower grammars.
    for (uint32_t i = 0; i < length;) {
        const auto first = static_cast<uint8_t>(text[i++]);
        if (first < 0x20 || first == 0x7f) return false;
        if (first < 0x80) continue;

        const uint32_t count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
        if (first < 0xc2 || first > 0xf4 || length - i < count) return false;
        const auto next = static_cast<uint8_t>(text[i]);
        if ((first == 0xe0 && next < 0xa0) || (first == 0xed && next > 0x9f) ||
            (first == 0xf0 && next < 0x90) || (first == 0xf4 && next > 0x8f))
            return false;
        for (uint32_t j = 0; j < count; ++j)
            if ((static_cast<uint8_t>(text[i++]) & 0xc0u) != 0x80u) return false;
    }
    return true;
}

inline const char* ParseUserLauncherEntry(const char* line, uint32_t length, UserLauncherEntry& out)
{
    if (length > UserLauncherRecordLimit) return "line too long";
    const char* fields[5] = {line};
    uint32_t sizes[5]{};
    uint32_t field = 0;
    for (uint32_t i = 0; i < length; ++i) {
        const char c = line[i];
        if (!c) return "embedded NUL";
        if (c == '\t') {
            if (++field == 5) return "expected five fields";
            fields[field] = line + i + 1;
        } else {
            ++sizes[field];
        }
    }
    if (field != 4) return "expected five fields";

    if (sizes[0] >= sizeof(out.title) || !UserLauncherValidTitle(fields[0], sizes[0])) return "invalid title";
    if (!UserLauncherBuildCommand(fields[1], sizes[1], out)) return "invalid command";
    if (sizes[2] >= sizeof(out.icon)) return "invalid icon name";
    for (uint32_t i = 0; i < sizes[2]; ++i) {
        const auto c = static_cast<uint8_t>(fields[2][i]);
        if (c < 0x20 || c > 0x7e || c == '/' || c == '\\' ||
            (c == '.' && (sizes[2] == 1 || (i + 1 < sizes[2] && fields[2][i + 1] == '.'))))
            return "invalid icon name";
    }
    if (sizes[3] >= sizeof(out.categories)) return "invalid categories";
    for (uint32_t i = 0; i < sizes[3]; ++i)
        if (!UserLauncherAsciiWord(fields[3][i]) && fields[3][i] != ';') return "invalid categories";
    if (!sizes[4] || sizes[4] >= sizeof(out.appId)) return "invalid app ID";
    for (uint32_t i = 0; i < sizes[4]; ++i)
        if (!UserLauncherAsciiWord(fields[4][i])) return "invalid app ID";

    char* outputs[] = {out.title, nullptr, out.icon, out.categories, out.appId};
    for (uint32_t f = 0; f < 5; ++f) {
        if (f == 1) continue;
        for (uint32_t i = 0; i < sizes[f]; ++i) outputs[f][i] = fields[f][i];
        outputs[f][sizes[f]] = 0;
    }
    return nullptr;
}

inline bool UserLauncherSameText(const char* a, const char* b)
{
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; }
    return !*a && !*b;
}

inline bool UserLauncherConflicts(const UserLauncherEntry& entry, const char* title, const char* appId)
{
    return UserLauncherSameText(entry.title, title) || UserLauncherSameText(entry.appId, appId);
}

struct UserLauncherCounts {
    uint32_t entries = 0;
    uint32_t rejected = 0;
};

template <class Add, class Reject>
UserLauncherCounts LoadUserLauncherRecords(const char* bytes, uint32_t length, Add add, Reject reject)
{
    UserLauncherCounts counts;
    uint32_t number = 0;
    for (uint32_t pos = 0; pos < length;) {
        const uint32_t first = pos;
        while (pos < length && bytes[pos] != '\n') ++pos;
        uint32_t size = pos - first;
        if (pos < length) ++pos;
        ++number;
        if (size && bytes[first + size - 1] == '\r') --size;
        if (!size) continue;

        UserLauncherEntry entry{};
        const char* error = ParseUserLauncherEntry(bytes + first, size, entry);
        if (!error && counts.entries == UserLauncherEntryLimit) error = "user entry limit";
        if (!error) error = add(entry);
        if (error) {
            ++counts.rejected;
            reject(number, error);
        } else {
            ++counts.entries;
        }
    }
    return counts;
}

class UserLauncherReload {
    uint64_t lastAccepted = 0;
    bool requested = false;
public:
    void Request() { __atomic_store_n(&requested, true, __ATOMIC_RELEASE); }

    // Only the launcher worker consumes requests. Ensure an update submitted
    // during the interval is coalesced into the next rebuild.
    bool Take(uint64_t now) {
        if (now < lastAccepted || !__atomic_exchange_n(&requested, false, __ATOMIC_ACQ_REL)) return false;
        lastAccepted = now + 1000;
        return true;
    }
};

} // namespace brook
