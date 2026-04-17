#pragma once

// Brook audio subsystem — minimal API for kernel audio output.
//
// An audio driver (e.g. Intel HDA) registers its playback functions
// during module init. The kernel can then use AudioPlay() etc.
// without knowing which driver is behind it.

#include <stdint.h>

namespace brook {

struct AudioDriver {
    const char* name;

    // Play PCM data.  Returns bytes queued, or negative on error.
    int  (*play)(const void* samples, uint32_t byteCount,
                 uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);

    // Stop playback.
    void (*stop)();

    // Is audio currently playing?
    bool (*isPlaying)();

    // Current playback position in bytes.
    uint32_t (*getPosition)();
};

// Register an audio driver.  Only one can be active at a time.
extern "C" bool AudioRegister(const AudioDriver* drv);

// Kernel-side convenience wrappers.
int      AudioPlay(const void* samples, uint32_t byteCount,
                   uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);
void     AudioStop();
bool     AudioIsPlaying();
uint32_t AudioGetPosition();
bool     AudioAvailable();

} // namespace brook
