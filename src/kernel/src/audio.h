#pragma once

// Brook audio subsystem — minimal API for kernel audio output.
//
// An audio driver (e.g. Intel HDA) registers its playback functions
// during module init. The kernel can then use AudioPlay() etc.
// without knowing which driver is behind it.
//
// AudioMixerSubmit() accumulates PCM from multiple /dev/dsp writers
// into a shared mix buffer. AudioMixerFlush() pushes the mixed result
// to the hardware driver.

#include <stdint.h>

namespace brook {

struct AudioDriver {
    const char* name;

    // Play PCM data.  Returns bytes queued, or negative on error.
    // If nonblock is true, return immediately if buffer is full (don't wait).
    int  (*play)(const void* samples, uint32_t byteCount,
                 uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample,
                 bool nonblock);

    // Stop playback.
    void (*stop)();

    // Is audio currently playing?
    bool (*isPlaying)();

    // Current playback position in bytes.
    uint32_t (*getPosition)();
};

// Register an audio driver.  Only one can be active at a time.
extern "C" bool AudioRegister(const AudioDriver* drv);

// Kernel-side convenience wrappers (direct to hardware — prefer mixer API).
int      AudioPlay(const void* samples, uint32_t byteCount,
                   uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample,
                   bool nonblock = false);
void     AudioStop();
bool     AudioIsPlaying();
uint32_t AudioGetPosition();
bool     AudioAvailable();

// --- Mixer API ---
// All samples must be 44100 Hz, stereo, 16-bit signed LE before submission.
// The mixer accumulates (adds with clamping) from multiple writers and
// flushes to hardware when enough data is ready.

static constexpr uint32_t MIXER_HW_RATE     = 44100;
static constexpr uint8_t  MIXER_HW_CHANNELS = 2;
static constexpr uint8_t  MIXER_HW_BITS     = 16;
static constexpr uint32_t MIXER_FRAME_BYTES  = MIXER_HW_CHANNELS * (MIXER_HW_BITS / 8); // 4

// Submit pre-resampled 44100/stereo/16-bit PCM into the mix buffer.
// Samples are added to whatever is already accumulated at that stream's position.
// streamId identifies the /dev/dsp fd (0-7), allowing independent write cursors.
void AudioMixerSubmit(const int16_t* samples, uint32_t frameCount, uint32_t streamId = 0);

// Flush the accumulated mix buffer to the hardware driver and reset.
void AudioMixerFlush();

// Initialize the mixer (call once at boot, after kmalloc is available).
void AudioMixerInit();

} // namespace brook
