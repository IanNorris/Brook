#ifndef BROOK_TASKBAR_H
#define BROOK_TASKBAR_H
#include <stdint.h>
enum {
    BROOK_SYS_WM_TASKBAR_METRICS = 0xB0C,
    BROOK_TASKBAR_OP_GET_METRICS = 0,
    BROOK_TASKBAR_OP_SET_METRICS = 1,
    BROOK_TASKBAR_OP_GET_ADVANCED_DETAILS = 2,
    BROOK_TASKBAR_OP_SET_ADVANCED_DETAILS = 3,
    BROOK_TASKBAR_ADVANCED_DETAILS_DEFAULT = 0,
    BROOK_TASKBAR_CPU = 1u,
    BROOK_TASKBAR_MEMORY = 2u,
    BROOK_TASKBAR_DISK = 4u,
    BROOK_TASKBAR_NETWORK = 8u,
    BROOK_TASKBAR_COMPOSITOR = 16u,
    BROOK_TASKBAR_APP = 32u,
    BROOK_TASKBAR_THERMAL = 64u,
    BROOK_TASKBAR_ALL = 127u,
    BROOK_TASKBAR_DEFAULT = BROOK_TASKBAR_CPU | BROOK_TASKBAR_MEMORY | BROOK_TASKBAR_THERMAL
};
static inline int brook_taskbar_metrics_valid(uint32_t value) { return !(value & ~127u); }
/* Strict bounded settings parser; preserve defaults for absent/invalid keys. */
static inline uint32_t brook_taskbar_metrics_setting(uint32_t prior, const char *line) {
    const char *key = "taskbar_metrics=";
    while (*key && *line == *key) { ++line; ++key; }
    if (*key) return prior;
    unsigned value = 0, digits = 0;
    while (*line >= '0' && *line <= '9' && digits < 3) {
        value = value * 10 + (*line++ - '0'); ++digits;
    }
    while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
    return digits && !*line && brook_taskbar_metrics_valid(value) ? value : prior;
}
#define BROOK_TASKBAR_ADVANCED_DETAILS_KEY "show_advanced_details"
static inline int brook_taskbar_advanced_details_valid(uint64_t value) { return value <= 1; }
static inline int brook_taskbar_advanced_details_setting(int prior, const char *line) {
    const char *key = BROOK_TASKBAR_ADVANCED_DETAILS_KEY "=";
    while (*key && *line == *key) { ++line; ++key; }
    if (*key || (*line != '0' && *line != '1')) return prior;
    int value = *line++ - '0';
    while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
    return *line ? prior : value;
}
#endif
