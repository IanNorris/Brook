#ifndef BROOK_POINTER_H
#define BROOK_POINTER_H
/* Touchpad pointer speed (percent of the default; 25..300).
 * syscall(BROOK_SYS_POINTER_SPEED, 0) reads it, (1, value) sets it live.
 * Persisted by Settings as [Desktop] pointer_speed=N and applied at boot. */
#include <stdint.h>
enum { BROOK_SYS_POINTER_SPEED = 0xB22, BROOK_POINTER_SPEED_DEFAULT = 100,
       BROOK_POINTER_SPEED_MIN = 25, BROOK_POINTER_SPEED_MAX = 300 };
static inline int brook_pointer_speed_valid(uint32_t v) {
    return v >= BROOK_POINTER_SPEED_MIN && v <= BROOK_POINTER_SPEED_MAX;
}
/* Strict bounded settings parser; preserves the prior value for absent/invalid keys. */
static inline uint32_t brook_pointer_speed_setting(uint32_t prior, const char *line) {
    const char *key = "pointer_speed=";
    while (*key && *line == *key) { ++line; ++key; }
    if (*key) return prior;
    unsigned value = 0, digits = 0;
    while (*line >= '0' && *line <= '9' && digits < 4) { value = value * 10 + (*line++ - '0'); ++digits; }
    while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
    return digits && !*line && brook_pointer_speed_valid(value) ? value : prior;
}

/* Touchpad tap-to-click (0 off, 1 on; default off).
 * syscall(BROOK_SYS_TOUCHPAD_TAP, 0) reads it, (1, 0|1) sets it live.
 * Persisted by Settings as [Desktop] touchpad_tap=0|1 and applied at boot. */
enum { BROOK_SYS_TOUCHPAD_TAP = 0xB23, BROOK_TOUCHPAD_TAP_DEFAULT = 0 };
static inline uint32_t brook_touchpad_tap_setting(uint32_t prior, const char *line) {
    const char *key = "touchpad_tap=";
    while (*key && *line == *key) { ++line; ++key; }
    if (*key || (*line != '0' && *line != '1')) return prior;
    const uint32_t value = (uint32_t)(*line++ - '0');
    while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
    return !*line ? value : prior;
}
#endif
