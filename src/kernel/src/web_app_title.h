#pragma once

#include <stdint.h>

namespace brook {

inline constexpr char WebAppTitleSeparator[] = " \xe2\x80\x93 ";

inline bool HasWebAppCategory(const char* categories)
{
    constexpr char tag[] = "X-Brook-WebApp";
    for (const char* token = categories; *token;) {
        const char* end = token;
        while (*end && *end != ';') ++end;
        if (uint32_t(end - token) == sizeof(tag) - 1) {
            uint32_t i = 0;
            while (i < sizeof(tag) - 1 && token[i] == tag[i]) ++i;
            if (i == sizeof(tag) - 1) return true;
        }
        token = *end ? end + 1 : end;
    }
    return false;
}

inline bool EqualWebAppTitle(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

inline bool IsBareWebAppHostname(const char* text)
{
    // A dotted DNS name, optional final root dot, or localhost. Distinguish
    // ordinary one-word page titles from hostnames. URLs and ports don't match.
    constexpr char local[] = "localhost";
    uint32_t i = 0;
    for (; text[i] && local[i]; ++i) {
        const char c = text[i];
        if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != local[i]) break;
    }
    if (!local[i] && (!text[i] || (text[i] == '.' && !text[i + 1]))) return true;

    bool dotted = false;
    uint32_t label = 0;
    char previous = 0;
    for (const char* p = text; *p; ++p) {
        const char c = *p;
        if (c == '.') {
            if (!label || previous == '-') return false;
            if (!p[1]) return dotted;
            dotted = true;
            label = 0;
        } else {
            const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9');
            if ((!alnum && (c != '-' || !label)) || ++label > 63) return false;
        }
        previous = c;
    }
    return dotted && previous != '-';
}

inline void FormatWebAppTitle(char (&out)[64], const char* name, const char* clientTitle)
{
    uint32_t used = 0;
    auto append = [&](const char* text) {
        while (*text && used + 1 < sizeof(out)) {
            const auto first = static_cast<uint8_t>(*text);
            const uint32_t bytes = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            if (used + bytes >= sizeof(out)) break;
            uint32_t available = 1;
            while (available < bytes && (static_cast<uint8_t>(text[available]) & 0xc0u) == 0x80u)
                ++available;
            if (available != bytes) break;
            for (uint32_t j = 0; j < bytes; ++j) out[used++] = *text++;
        }
    };
    append(name);
    if (*clientTitle && !EqualWebAppTitle(clientTitle, name) && !IsBareWebAppHostname(clientTitle)) {
        append(WebAppTitleSeparator);
        append(clientTitle);
    }
    out[used] = 0;
}

} // namespace brook
