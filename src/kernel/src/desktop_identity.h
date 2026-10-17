#pragma once
#include <stdint.h>

namespace brook {
// Named compatibility identity for the existing persistent desktop home.
// This is launch metadata, not a login or an authorization boundary.
inline constexpr uint32_t DesktopUid = 1000;
inline constexpr uint32_t DesktopGid = 100;
inline constexpr char DesktopPasswd[] =
    "root:x:0:0:root:/data/home:/boot/BIN/BASH\n"
    "brook:x:1000:100:Brook:/data/home:/boot/BIN/BASH\n";
inline constexpr char DesktopGroup[] = "root:x:0:\nusers:x:100:brook\n";

inline bool ParseLaunchId(const char* text, uint32_t* result) {
    if (!text || !*text) return false;
    uint32_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        uint32_t digit = static_cast<uint32_t>(*text - '0');
        if (value > (0xfffffffeU - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *result = value;
    return true;
}
inline uint32_t LaunchPrimaryGroup(uint32_t uid) {
    return uid == DesktopUid ? DesktopGid : uid;
}
// Output is bounded by the prefix plus ten decimal digits and a terminator.
// The caller supplies a 32-byte buffer; unknown numeric UIDs never say root.
inline void LaunchIdentityEnvironment(char (&out)[32], bool login, uint32_t uid) {
    const char* prefix = login ? "LOGNAME=" : "USER=";
    uint32_t pos = 0;
    while (*prefix) out[pos++] = *prefix++;
    const char* name = uid == 0 ? "root" : uid == DesktopUid ? "brook" : nullptr;
    if (name) while (*name) out[pos++] = *name++;
    else {
        char digits[10]; uint32_t count = 0;
        do { digits[count++] = '0' + uid % 10; uid /= 10; } while (uid);
        while (count) out[pos++] = digits[--count];
    }
    out[pos] = 0;
}
}
