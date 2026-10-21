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
#include "audio_service_timing.h"
#include "audio_delivery_timing.h"
#include "audio_mix_contributions.h"

namespace brook {
#ifdef BROOK_HOST_TEST
// Deterministic hosted experiment; not exported to the guest or modules.
void AudioMixerTestCommitQuantum(uint32_t frames);
uint64_t AudioMixerTestCommitCount();
#endif

// Native clocked submission: all byte watermarks describe 44.1k stereo S16.
// A receipt is one bounded, nonblocking interval in one hardware epoch.
// A nonzero accepted count plus error marks exposed data with uncertain delivery;
// only the terminal progress watermark may credit its completion.
struct AudioSubmitReceipt {
    uint64_t epoch = 0;
    uint64_t begin = 0;
    uint32_t accepted = 0;
    int error = 0;
};
// Device watermarks include unowned zero-filled physical intervals. Completion
// can pass the last submitted application byte while DMA drains over silence.
struct AudioDeviceProgress {
    uint64_t epoch = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint32_t capacity = 0;
    bool running = false;
    bool discontinuity = false;
};

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

    // Optional native clock contract. Snapshot returns false if no trustworthy
    // elapsed-time source exists. Terminal completion/fault stays observable
    // until explicit epoch restart; new submissions reject a latched fault.
    AudioSubmitReceipt (*trySubmit)(const void*, uint32_t) = nullptr;
    bool (*progress)(AudioDeviceProgress*) = nullptr;
    bool (*restart)(uint64_t expectedEpoch) = nullptr;
};

// Zero means no trustworthy elapsed-time source (legacy playback still works).
extern "C" uint64_t AudioClockNowNs();

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
// Empty streams may negotiate a smaller capacity; reset preserves geometry.
bool AudioMixerSetCapacity(AudioStreamId stream, uint32_t frames, uint32_t writableFrames = 1);
// Readiness threshold only; may change on an active stream without resetting
// its queue, playback epoch or completion clock.
bool AudioMixerSetWritableFrames(AudioStreamId stream, uint32_t frames);

struct AudioStreamProgress {
    uint64_t epoch = 0;
    uint64_t submittedFrames = 0;
    uint64_t completedFrames = 0;
    uint32_t queuedFrames = 0;
    bool clocked = false;
    bool discontinuity = false;
    uint32_t capacityFrames = MIXER_RING_FRAMES;
    uint32_t writableFrames = 1;
};
// Refresh calls the driver without the mixer lock. GetProgress reads cached
// accounting only and is safe for descriptor scans that cannot sleep.
void AudioMixerRefresh();
bool AudioMixerGetProgress(AudioStreamId stream, AudioStreamProgress* result);

// Read-only diagnostics: one bounded cached-mixer snapshot. No driver poll,
// refresh, allocation or sample consumption. The clock is sampled separately.
struct AudioStreamDiagnostic {
    AudioStreamId identity = 0;
    uint64_t epoch = 0, submittedFrames = 0, completedFrames = 0;
    uint32_t softwareFrames = 0, inFlightFrames = 0, capacityFrames = 0;
    bool enabled = false, discontinuity = false;
};
struct AudioMixerDiagnostic {
    uint64_t clockNs = 0;
    bool ready = false, clocked = false;
    AudioStreamDiagnostic streams[MIXER_MAX_STREAMS]{};
};
void AudioMixerDiagnosticSnapshot(AudioMixerDiagnostic* result);

// Default-off accepted native PCM attribution; no driver poll or clock read.
void AudioMixerContributionsEnable(bool enabled);
void AudioMixerContributionsReset();
AudioMixContributionSnapshot AudioMixerContributionsRead();

// Experimental shared-positive-prefix batching for fixed intel_hda 12-KiB
// publication lead. No additional wait or submission loop; default off.
struct AudioMixerSharedPrefixSnapshot {
    uint64_t epoch=0, candidates=0, shortenedAttempts=0, shortenedReceipts=0;
    uint64_t acceptedPrefixFrames=0, requestedTailFrames=0;
    uint64_t headroomBypasses=0, invalidClock=0, unsupportedDriver=0;
    bool enabled=false, saturated=false, exhausted=false;
};
void AudioMixerSharedPrefixEnable(bool enabled);
void AudioMixerSharedPrefixReset();
AudioMixerSharedPrefixSnapshot AudioMixerSharedPrefixRead();

// Experimental, default-off refill coalescing for the unchanged intel_hda
// 12-KiB publication lead. Not a generic driver latency guarantee.
struct AudioMixerCoalesceSnapshot {
    bool enabled = false, saturated = false;
    uint64_t deferrals = 0, recoveredContributors = 0, forcedResumes = 0;
    uint64_t headroomBypasses = 0, invalidClock = 0, unsupportedDriver = 0;
    uint64_t lateResumes = 0;
};
void AudioMixerCoalesceEnable(bool enabled);
void AudioMixerCoalesceReset();
AudioMixerCoalesceSnapshot AudioMixerCoalesceRead();

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
