#ifndef BROOK_AUDIO_CONTROL_H
#define BROOK_AUDIO_CONTROL_H
#include <stdint.h>
enum {
    BROOK_SYS_AUDIO_CONTROL = 0xB0D,
    BROOK_AUDIO_MUTED = 0x100,
    BROOK_AUDIO_DEFAULT = 100,
    BROOK_AUDIO_PROBE_DEFAULT = BROOK_AUDIO_MUTED | 20
};
/* One atomic value: linear PCM amplitude percent (0..100), plus mute.
 * Operations: 0=query, 1=set, 2=device available. No amplification above unity.
 * Already queued hardware samples drain before a new value becomes audible. */
static inline int brook_audio_control_valid(uint32_t value) {
    return !(value & ~0x17fu) && (value & 0x7fu) <= 100;
}
static inline unsigned brook_audio_volume(uint32_t value) { return value & 0x7fu; }
static inline uint32_t brook_audio_control_setting(uint32_t prior,const char *line) {
    const char *key="output=";
    while (*key && *line==*key) { ++line; ++key; }
    if (*key) return prior;
    unsigned value=0,digits=0;
    while (*line>='0' && *line<='9' && digits<4) { value=value*10+(*line++-'0'); ++digits; }
    while (*line==' ' || *line=='\t' || *line=='\r') ++line;
    return digits && !*line && brook_audio_control_valid(value)?value:prior;
}
static inline int16_t brook_audio_scale(int32_t sample,uint32_t control) {
    int32_t scaled = (control & BROOK_AUDIO_MUTED) ? 0 :
        sample * (int32_t)brook_audio_volume(control) / 100;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}
#endif
