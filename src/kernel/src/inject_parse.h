#pragma once
// Parsing for the debug channel's `inject` verbs that carry signed or named arguments
// (net.cpp). Pure, host-tested in test_inject_parse.
//   inject button <0|1|2> down|up   left / right / middle
//   inject rel <dx> <dy>            relative pointer motion (signed, |d| <= 4096)
#include <stdint.h>

namespace brook {

// Parses an optionally signed decimal at s; returns characters consumed (0 = none).
inline int InjectParseInt(const char* s, int32_t* out) {
    int i = 0; bool neg = false;
    if (s[i] == '-' || s[i] == '+') { neg = s[i] == '-'; ++i; }
    const int digits = i;
    int64_t v = 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); if (v > 1000000) return 0; ++i; }
    if (i == digits) return 0;
    *out = static_cast<int32_t>(neg ? -v : v);
    return i;
}

// "<dx> <dy>" with |d| <= 4096 and nothing after.
inline bool InjectParseRel(const char* s, int32_t* dx, int32_t* dy) {
    int n = InjectParseInt(s, dx);
    if (!n || s[n] != ' ') return false;
    const int m = InjectParseInt(s + n + 1, dy);
    if (!m || s[n + 1 + m]) return false;
    return *dx >= -4096 && *dx <= 4096 && *dy >= -4096 && *dy <= 4096;
}

// "<0|1|2> down|up" -> button index and direction.
inline bool InjectParseButton(const char* s, uint8_t* button, bool* down) {
    if (s[0] < '0' || s[0] > '2' || s[1] != ' ') return false;
    *button = static_cast<uint8_t>(s[0] - '0');
    const char* w = s + 2;
    if (w[0] == 'd' && w[1] == 'o' && w[2] == 'w' && w[3] == 'n' && !w[4]) { *down = true; return true; }
    if (w[0] == 'u' && w[1] == 'p' && !w[2]) { *down = false; return true; }
    return false;
}

} // namespace brook
