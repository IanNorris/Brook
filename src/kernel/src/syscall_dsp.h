#pragma once

// syscall_dsp.h -- internal declarations shared from syscall_dsp.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "sync/kmutex.h"
#include "audio.h"

namespace brook {

// DspState defined here so DspHandleRef can access refCount.
// /dev/dsp ioctl constants and handling code are further below.
struct DspState {
    uint32_t sampleRate;
    uint8_t  channels;
    uint8_t  bitsPerSample;
    uint32_t fragmentSize;   // bytes per fragment
    uint32_t ringBytes;      // zero = default geometry; explicit geometry is native-rate only
    uint32_t bufferOffset;   // write cursor into staging buffer
    uint8_t* buffer;         // staging buffer (kmalloc'd)
    uint32_t bufferSize;     // total staging buffer size
    AudioStreamId mixerStreamId; // generation-tagged mixer ownership
    uint64_t pointerEpoch;    // GETOPTR origin after reset/format negotiation
    uint64_t pointerBaseFrames;
    uint64_t controlEpoch;    // cancels drains on reset, pause or reconfiguration
    KMutex ioLock;           // shared by dup/fork; never held while waiting for FIFO space
    uint8_t* inputBuffer;    // bounded user-copy staging
    uint32_t refCount;       // fork-safe refcount (1 on creation)
};

struct DspIoGuard {
    DspState* state;
    explicit DspIoGuard(DspState* value) : state(value) { KMutexLock(&state->ioLock); }
    ~DspIoGuard() { KMutexUnlock(&state->ioLock); }
};

bool DspResetStreamLocked(DspState* dsp);
bool DspRecoverStreamLocked(DspState* dsp,AudioStreamProgress& progress);

// OSS ioctl numbers (from <sys/soundcard.h>)
static constexpr uint64_t SNDCTL_DSP_RESET     = 0x5000;

static constexpr uint64_t SNDCTL_DSP_SYNC      = 0x5001;
static constexpr uint64_t SNDCTL_DSP_SPEED     = 0xC0045002; // _IOWR('P', 2, int)
static constexpr uint64_t SNDCTL_DSP_SETFMT    = 0xC0045005; // _IOWR('P', 5, int)
static constexpr uint64_t SNDCTL_DSP_CHANNELS  = 0xC0045006; // _IOWR('P', 6, int)
static constexpr uint64_t SNDCTL_DSP_STEREO    = 0xC0045003; // _IOWR('P', 3, int)
static constexpr uint64_t SNDCTL_DSP_POST      = 0x5008;
static constexpr uint64_t SNDCTL_DSP_GETOSPACE = 0x8010500C; // _IOR('P', 12, audio_buf_info), 16-byte payload
static constexpr uint64_t SNDCTL_DSP_GETODELAY = 0x80045017; // queued output bytes
static constexpr uint64_t SNDCTL_DSP_GETOPTR  = 0x800C5012; // count_info, 12-byte payload
static constexpr uint64_t SNDCTL_DSP_GETCAPS   = 0x8004500F; // _IOR('P', 0x0F, int)
static constexpr uint64_t SNDCTL_DSP_SETTRIGGER= 0x40045010; // _IOW('P', 0x10, int)
static constexpr uint64_t SNDCTL_DSP_GETFMTS   = 0x8004500B; // _IOR('P', 0x0B, int)
static constexpr uint64_t SNDCTL_DSP_SETFRAGMENT= 0xC004500A;// _IOWR('P', 0x0A, int)
static constexpr uint64_t SNDCTL_DSP_GETBLKSIZE = 0xC0045004;// _IOWR('P', 4, int)
static constexpr uint64_t SNDCTL_DSP_LOW_WATER = 0x40045022;// OSS4 _IOW('P', 34, int), poll threshold in input bytes

// OSS audio formats
static constexpr int AFMT_U8     = 0x00000008;

static constexpr int AFMT_S16_LE = 0x00000010;

// OSS capabilities
static constexpr int DSP_CAP_TRIGGER = 0x00001000;

// DspState is defined earlier in the file (near DspHandleRef).

static constexpr uint32_t DSP_DEFAULT_RATE     = 44100;

static constexpr uint8_t  DSP_DEFAULT_CHANNELS = 2;
static constexpr uint8_t  DSP_DEFAULT_BITS     = 16;
static constexpr uint32_t DSP_HW_RATE          = 44100; // hardware playback rate
static constexpr uint32_t DSP_BUFFER_SIZE      = 65536; // 64KB staging buffer

int DspChangeFormat(DspState* dsp, const AudioStreamProgress& progress,
                    uint32_t rate, uint8_t channels, uint8_t bits);

} // namespace brook
