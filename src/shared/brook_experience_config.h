#pragma once
#include <stdint.h>

#define BROOK_EXPERIENCE_GROUP "Experience"
#define BROOK_EXPERIENCE_DETAILS_KEY "technical_details"
enum { BROOK_EXPERIENCE_PRO_MODE_DEFAULT = 0 };

static inline const char *brook_experience_skip_ini_space(const char *line) {
    while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
    return line;
}

static inline int brook_experience_ini_section(const char *line) {
    const char *name = "[" BROOK_EXPERIENCE_GROUP "]";
    while (*name && *line == *name) { ++line; ++name; }
    return !*name && !*brook_experience_skip_ini_space(line);
}

static inline int brook_experience_pro_mode_valid(uint64_t value) { return value <= 1; }

/* Read the same boolean representation as the message helper's GKeyFile. */
static inline int brook_experience_pro_mode_setting(int prior, const char *line) {
    const char *key = BROOK_EXPERIENCE_DETAILS_KEY;
    while (*key && *line == *key) { ++line; ++key; }
    if (*key) return prior;
    line = brook_experience_skip_ini_space(line);
    if (*line != '=') return prior;
    line = brook_experience_skip_ini_space(line + 1);
    if (*line == '1' && !*brook_experience_skip_ini_space(line + 1)) return 1;
    const char *enabled = "true";
    while (*enabled && *line == *enabled) { ++line; ++enabled; }
    return !*enabled && !*brook_experience_skip_ini_space(line);
}
