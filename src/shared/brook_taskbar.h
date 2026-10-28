#ifndef BROOK_TASKBAR_H
#define BROOK_TASKBAR_H
#include <stdint.h>
enum {
    BROOK_SYS_WM_TASKBAR_METRICS = 0xB0C,
    BROOK_TASKBAR_CPU = 1u,
    BROOK_TASKBAR_MEMORY = 2u,
    BROOK_TASKBAR_DISK = 4u,
    BROOK_TASKBAR_NETWORK = 8u,
    BROOK_TASKBAR_COMPOSITOR = 16u,
    BROOK_TASKBAR_APP = 32u,
    BROOK_TASKBAR_ALL = 63u,
    BROOK_TASKBAR_DEFAULT = BROOK_TASKBAR_CPU | BROOK_TASKBAR_MEMORY
};
static inline int brook_taskbar_metrics_valid(uint32_t value) { return !(value & ~63u); }
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
#endif
