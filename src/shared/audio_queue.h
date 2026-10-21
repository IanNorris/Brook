#pragma once
#include <stdint.h>

/* Brook native OSS extension: one coherent per-open-description snapshot.
 * All counts are 44100-Hz stereo S16 frames, not bytes or converted frames.
 * available_frames describes reusable client storage; queued_frames includes
 * downstream source which has not physically completed. Neither is a clock. */
#define BROOK_DSP_GETQUEUE 0x80384270UL
#define BROOK_AUDIO_QUEUE_VERSION 1U
#define BROOK_AUDIO_QUEUE_CLOCKED 1U
#define BROOK_AUDIO_QUEUE_DISCONTINUITY 2U
struct BrookAudioQueue {
    uint32_t version, flags;
    uint64_t epoch, submitted_frames, completed_frames;
    uint32_t capacity_frames, available_frames, queued_frames, staging_frames;
    uint32_t rate, frame_bytes;
};
#ifdef __cplusplus
static_assert(sizeof(BrookAudioQueue)==56,"Brook audio queue ABI");
#else
_Static_assert(sizeof(struct BrookAudioQueue)==56,"Brook audio queue ABI");
#endif
