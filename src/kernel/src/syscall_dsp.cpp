// syscall_dsp.cpp -- OSS /dev/dsp descriptor state and format helpers.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "dsp_write_plan.h"
#include "syscall_internal.h"
#include "syscall_dsp.h"

namespace brook {

// The OSS facade prepares again after an underrun, as Linux's OSS emulation
// does. Retire the old stream epoch; discarded frames are never completed.
// Caller holds this open description's ioLock.
bool DspResetStreamLocked(DspState* dsp)
{
    if(!AudioMixerReset(dsp->mixerStreamId))return false;
    dsp->bufferOffset=0;dsp->pointerEpoch=dsp->pointerBaseFrames=0;
    ++dsp->controlEpoch;return true;
}
bool DspRecoverStreamLocked(DspState* dsp,AudioStreamProgress& progress)
{
    if(!progress.discontinuity)return true;
    return DspResetStreamLocked(dsp) && AudioMixerGetProgress(dsp->mixerStreamId,&progress);
}

void DspHandleRef(void* handle)
{
    if (!handle) return;
    auto* dsp = static_cast<DspState*>(handle);
    __atomic_fetch_add(&dsp->refCount, 1, __ATOMIC_ACQ_REL);
}

// ---------------------------------------------------------------------------
// OSS /dev/dsp state and ioctl definitions
// ---------------------------------------------------------------------------

static_assert(DSP_BUFFER_SIZE == DspWritePlan::InputBytesLimit && DSP_HW_RATE == DspWritePlan::HardwareRate);

// Caller holds ioLock and has checked that no input remains outstanding.
int DspChangeFormat(DspState* dsp, const AudioStreamProgress& progress,
                    uint32_t rate, uint8_t channels, uint8_t bits)
{
    if (dsp->ringBytes && rate != MIXER_HW_RATE) return -95;
    const uint32_t frames = dsp->ringBytes ? dsp->ringBytes / (channels * (bits / 8)) : MIXER_RING_FRAMES;
    if (!frames || frames > MIXER_RING_FRAMES) return -EINVAL;
    uint64_t period = (uint64_t(dsp->fragmentSize) * MIXER_HW_RATE + uint64_t(channels) * (bits / 8) * rate - 1)
        / (uint64_t(channels) * (bits / 8) * rate);
    if (period > frames) period = frames;
    if (!AudioMixerSetCapacity(dsp->mixerStreamId, frames, static_cast<uint32_t>(period))) return -16;
    ++dsp->controlEpoch;
    dsp->pointerEpoch = progress.epoch; dsp->pointerBaseFrames = progress.completedFrames;
    dsp->sampleRate = rate; dsp->channels = channels; dsp->bitsPerSample = bits;
    return 0;
}

} // namespace brook
