#pragma once

// Brook audio subsystem — minimal API for kernel audio output.
//
// An audio driver (e.g. Intel HDA) registers its playback functions
// during module init. The kernel can then use AudioPlay() etc.
// without knowing which driver is behind it.
//
// AudioMixerSubmit() queues PCM for an independently owned stream.
// AudioMixerFlush() mixes streams when delivering a batch to the driver.

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

    // Periodic maintenance, including draining an idle DMA queue. Called in
    // thread context even when applications have stopped submitting samples.
    void (*service)() = nullptr;
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
// Queues retain unaccepted samples; mixing/clamping happens at delivery.

static constexpr uint32_t MIXER_HW_RATE     = 44100;
static constexpr uint8_t  MIXER_HW_CHANNELS = 2;
static constexpr uint8_t  MIXER_HW_BITS     = 16;
static constexpr uint32_t MIXER_FRAME_BYTES  = MIXER_HW_CHANNELS * (MIXER_HW_BITS / 8); // 4
static constexpr uint32_t MIXER_RING_FRAMES = 16384;

using AudioStreamId = uint64_t;
static constexpr uint32_t MIXER_MAX_STREAMS = 8;
// Zero means unavailable/exhausted. Identities include slot generations.
AudioStreamId AudioMixerOpen();
bool AudioMixerClose(AudioStreamId stream);
bool AudioMixerReset(AudioStreamId stream);
bool AudioMixerSetEnabled(AudioStreamId stream, bool enabled);

// Submit pre-resampled 44100/stereo/16-bit PCM into this stream's FIFO.
// Returns the number of frames actually accepted; callers must retry the rest.
uint32_t AudioMixerSubmit(const int16_t* samples, uint32_t frameCount, AudioStreamId stream);

// Query available space in mixer ring for a given stream (in frames).
uint32_t AudioMixerAvailableFrames(AudioStreamId stream);

// Deliver a pending batch or move accumulated samples into a new batch.
// Retain the unaccepted suffix across zero, partial and failed driver writes.
// Concurrent flush calls leave the current delivery owner in charge.
void AudioMixerFlush();

// Initialize the mixer (call once at boot, after kmalloc is available).
void AudioMixerInit();

// Start the mixer drain thread (call after scheduler is running).
void AudioMixerThreadStart();

// Periodic tick — called from mixer thread every ~10ms.
// Drains accumulated mix data to the hardware in consistent chunks.
void AudioMixerTick();

} // namespace brook
