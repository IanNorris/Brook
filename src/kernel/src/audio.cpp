// audio.cpp — Brook audio subsystem registry + software mixer.
//
// Each /dev/dsp open owns a FIFO of resampled 44100 Hz stereo 16-bit PCM.
// Delivery snapshots mix live streams without sharing write cursors.
// A dedicated kernel thread (audio_mix) wakes every ~10ms and drains
// accumulated data to the hardware driver in consistent chunks,
// decoupling application write timing from hardware playback timing.

#include "audio.h"
#include "serial.h"
#include "kprintf.h"
#include "memory/heap.h"
#include "spinlock.h"
#include "string.h"
#include "scheduler.h"
#include "process.h"
#include "apic.h"

namespace brook {

extern volatile uint64_t g_lapicTickCount;

static const AudioDriver* g_audioDriver = nullptr;
static SpinLock g_mixerLock;

// Independently owned FIFO rings. Samples remain charged to their stream
// until the driver accepts them; prepared mixes never own pointers into a ring.
static constexpr uint32_t MIXER_BUF_MASK = MIXER_RING_FRAMES - 1;
static constexpr uint32_t MIXER_BATCH_FRAMES = 1764;
struct MixerStream {
    int16_t* samples = nullptr;
    AudioStreamId identity = 0;
    uint64_t epoch = 0;
    uint64_t read = 0, write = 0;
    bool enabled = true;
};
static MixerStream g_streams[MIXER_MAX_STREAMS];
static uint64_t g_nextStreamIdentity = 1;
static bool g_mixerReady = false;
static int16_t* g_flushBuf = nullptr;
static uint32_t g_flushBusy = 0;
static uint64_t g_deliveryEpoch = 0, g_flushEpoch = 0;
static uint32_t g_batchRemaining = 0;
struct Contribution {
    AudioStreamId identity = 0;
    uint64_t epoch = 0, read = 0;
    uint32_t frames = 0;
};
// If a generic driver accepts part of an encoded stereo frame, preserve the
// rest of that frame exactly. Later whole frames can be remixed after a close.
static uint8_t g_partialFrame[MIXER_FRAME_BYTES];
static uint32_t g_partialOffset = 0;
static Contribution g_partialContributions[MIXER_MAX_STREAMS];

static MixerStream* FindStream(AudioStreamId identity)
{
    const uint32_t slot = static_cast<uint32_t>(identity & 15);
    if (!slot || slot > MIXER_MAX_STREAMS) return nullptr;
    auto& stream = g_streams[slot - 1];
    return identity && stream.identity == identity ? &stream : nullptr;
}
static void ResetStream(MixerStream& stream)
{
    ++stream.epoch;
    stream.read = stream.write = 0;
}
static void ResetAllStreams()
{
    SpinLockAcquire(&g_mixerLock);
    for (auto& stream : g_streams) ResetStream(stream);
    SpinLockRelease(&g_mixerLock);
}
void AudioMixerInit()
{
    // Boot-only initialization: allocate outside the mixer lock.
    for (auto& stream : g_streams)
        stream.samples = static_cast<int16_t*>(kmalloc(MIXER_RING_FRAMES * MIXER_FRAME_BYTES));
    g_flushBuf = static_cast<int16_t*>(kmalloc(MIXER_BATCH_FRAMES * MIXER_FRAME_BYTES));
    bool ready = g_flushBuf != nullptr;
    for (const auto& stream : g_streams) ready = ready && stream.samples;
    if (!ready) {
        for (auto& stream : g_streams) { kfree(stream.samples); stream.samples = nullptr; }
        kfree(g_flushBuf); g_flushBuf = nullptr;
        return;
    }
    g_mixerReady = true;
}
AudioStreamId AudioMixerOpen()
{
    SpinLockAcquire(&g_mixerLock);
    AudioStreamId identity = 0;
    if (g_mixerReady && g_nextStreamIdentity <= (~uint64_t(0) >> 4)) {
        for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
            auto& stream = g_streams[i];
            if (stream.identity) continue;
            identity = (g_nextStreamIdentity++ << 4) | (i + 1);
            ResetStream(stream); stream.enabled = true; stream.identity = identity;
            break;
        }
    }
    SpinLockRelease(&g_mixerLock);
    return identity;
}
bool AudioMixerClose(AudioStreamId identity)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (stream) { ResetStream(*stream); stream->identity = 0; }
    SpinLockRelease(&g_mixerLock);
    return stream != nullptr;
}
bool AudioMixerReset(AudioStreamId identity)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (stream) ResetStream(*stream);
    SpinLockRelease(&g_mixerLock);
    return stream != nullptr;
}
bool AudioMixerSetEnabled(AudioStreamId identity, bool enabled)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (stream) stream->enabled = enabled;
    SpinLockRelease(&g_mixerLock);
    return stream != nullptr;
}
uint32_t AudioMixerAvailableFrames(AudioStreamId identity)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    uint32_t available = stream ? MIXER_RING_FRAMES - static_cast<uint32_t>(stream->write - stream->read) : 0;
    SpinLockRelease(&g_mixerLock);
    return available;
}
uint32_t AudioMixerSubmit(const int16_t* samples, uint32_t frames, AudioStreamId identity)
{
    if (!samples || !frames) return 0;
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (!stream) { SpinLockRelease(&g_mixerLock); return 0; }
    const uint32_t available = MIXER_RING_FRAMES - static_cast<uint32_t>(stream->write - stream->read);
    if (frames > available) frames = available;
    // Empty queues may restart their indices, avoiding lifetime counter wrap.
    if (stream->read == stream->write) stream->read = stream->write = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t index = static_cast<uint32_t>(stream->write + i) & MIXER_BUF_MASK;
        stream->samples[index * 2] = samples[i * 2];
        stream->samples[index * 2 + 1] = samples[i * 2 + 1];
    }
    stream->write += frames;
    SpinLockRelease(&g_mixerLock);
    return frames;
}
static void Consume(const Contribution* contributions, uint32_t frames)
{
    for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
        const auto& part = contributions[i];
        auto* stream = FindStream(part.identity);
        if (!stream || stream->epoch != part.epoch || stream->read != part.read) continue;
        uint32_t consumed = frames < part.frames ? frames : part.frames;
        stream->read += consumed;
    }
}
// Snapshot a new whole-frame attempt. Rebuilding unaccepted frames removes
// closed/reset/paused contributors without subtracting already-clamped sums.
static uint32_t Prepare(Contribution* contributions)
{
    uint32_t frames = 0;
    for (const auto& stream : g_streams)
        if (stream.identity && stream.enabled && stream.write - stream.read > frames)
            frames = static_cast<uint32_t>(stream.write - stream.read);
    if (frames > MIXER_BATCH_FRAMES) frames = MIXER_BATCH_FRAMES;
    if (g_batchRemaining && frames > g_batchRemaining) frames = g_batchRemaining;
    g_batchRemaining = frames;
    for (uint32_t slot = 0; slot < MIXER_MAX_STREAMS; ++slot) {
        const auto& stream = g_streams[slot];
        if (!stream.identity || !stream.enabled) continue;
        uint32_t count = static_cast<uint32_t>(stream.write - stream.read);
        if (count > frames) count = frames;
        contributions[slot] = {stream.identity, stream.epoch, stream.read, count};
    }
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t left = 0, right = 0;
        for (uint32_t slot = 0; slot < MIXER_MAX_STREAMS; ++slot) {
            const auto& part = contributions[slot];
            if (i >= part.frames) continue;
            const uint32_t index = static_cast<uint32_t>(part.read + i) & MIXER_BUF_MASK;
            left += g_streams[slot].samples[index * 2];
            right += g_streams[slot].samples[index * 2 + 1];
        }
        if (left > 32767) left = 32767; else if (left < -32768) left = -32768;
        if (right > 32767) right = 32767; else if (right < -32768) right = -32768;
        g_flushBuf[i * 2] = static_cast<int16_t>(left);
        g_flushBuf[i * 2 + 1] = static_cast<int16_t>(right);
    }
    return frames;
}
void AudioMixerFlush()
{
    if (__atomic_exchange_n(&g_flushBusy, 1u, __ATOMIC_ACQUIRE)) return;
    struct FlushOwner { ~FlushOwner() { __atomic_store_n(&g_flushBusy, 0u, __ATOMIC_RELEASE); } } owner;
    const uint64_t epoch = __atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE);
    if (g_flushEpoch != epoch) { g_batchRemaining = g_partialOffset = 0; g_flushEpoch = epoch; }
    Contribution contributions[MIXER_MAX_STREAMS]{};
    const bool partial = g_partialOffset != 0;
    SpinLockAcquire(&g_mixerLock);
    if (!g_mixerReady || !g_audioDriver || !g_audioDriver->play) {
        SpinLockRelease(&g_mixerLock); return;
    }
    const uint32_t frames = partial ? 0 : Prepare(contributions);
    SpinLockRelease(&g_mixerLock);
    if (!partial && !frames) return;
    if (__atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE) != epoch) {
        g_batchRemaining = g_partialOffset = 0; return;
    }
    const uint32_t bytes = partial ? MIXER_FRAME_BYTES - g_partialOffset : frames * MIXER_FRAME_BYTES;
    const void* data = partial ? static_cast<const void*>(g_partialFrame + g_partialOffset) : g_flushBuf;
    const int accepted = g_audioDriver->play(data, bytes,
        MIXER_HW_RATE, MIXER_HW_CHANNELS, MIXER_HW_BITS, false);
    if (__atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE) != epoch) {
        g_batchRemaining = g_partialOffset = 0; return;
    }
    if (accepted <= 0) return;
    if (static_cast<uint32_t>(accepted) > bytes) {
        SerialPrintf("audio: driver returned %d for %u pending bytes\n", accepted, bytes); return;
    }
    SpinLockAcquire(&g_mixerLock);
    if (partial) {
        g_partialOffset += static_cast<uint32_t>(accepted);
        if (g_partialOffset == MIXER_FRAME_BYTES) {
            Consume(g_partialContributions, 1);
            g_partialOffset = 0; --g_batchRemaining;
        }
    } else {
        const uint32_t whole = static_cast<uint32_t>(accepted) / MIXER_FRAME_BYTES;
        Consume(contributions, whole);
        g_batchRemaining -= whole;
        const uint32_t tail = static_cast<uint32_t>(accepted) % MIXER_FRAME_BYTES;
        if (tail) {
            memcpy(g_partialFrame, reinterpret_cast<uint8_t*>(g_flushBuf) + whole * MIXER_FRAME_BYTES, MIXER_FRAME_BYTES);
            g_partialOffset = tail;
            for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
                const auto& part = contributions[i];
                g_partialContributions[i] = part.frames > whole
                    ? Contribution{part.identity, part.epoch, part.read + whole, 1} : Contribution{};
            }
        }
    }
    SpinLockRelease(&g_mixerLock);
}

void AudioMixerTick()
{
    if (!g_mixerReady) return;
    AudioMixerFlush();
    if (g_audioDriver && g_audioDriver->service)
        g_audioDriver->service();
}

// --- Driver registration ---

extern "C" bool AudioRegister(const AudioDriver* drv)
{
    if (g_audioDriver)
    {
        SerialPrintf("audio: driver '%s' already registered, rejecting '%s'\n",
                     g_audioDriver->name, drv->name);
        return false;
    }
    g_audioDriver = drv;
    KPrintf("audio: registered driver '%s'\n", drv->name);
    return true;
}

bool AudioAvailable()
{
    return g_audioDriver != nullptr;
}

int AudioPlay(const void* samples, uint32_t byteCount,
              uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample,
              bool nonblock)
{
    if (!g_audioDriver || !g_audioDriver->play) return -1;
    return g_audioDriver->play(samples, byteCount, sampleRate, channels, bitsPerSample, nonblock);
}

void AudioStop()
{
    // Invalidate retained delivery at both boundaries: a batch can be formed
    // while the sleeping driver stop is in progress. Only its flush owner
    // mutates the batch; this does not retract an in-flight hardware write.
    __atomic_fetch_add(&g_deliveryEpoch, 1ULL, __ATOMIC_ACQ_REL);
    ResetAllStreams();
    if (g_audioDriver && g_audioDriver->stop)
        g_audioDriver->stop();
    ResetAllStreams();
    __atomic_fetch_add(&g_deliveryEpoch, 1ULL, __ATOMIC_ACQ_REL);
}

bool AudioIsPlaying()
{
    if (!g_audioDriver || !g_audioDriver->isPlaying) return false;
    return g_audioDriver->isPlaying();
}

uint32_t AudioGetPosition()
{
    if (!g_audioDriver || !g_audioDriver->getPosition) return 0;
    return g_audioDriver->getPosition();
}

// --- Mixer kernel thread ---
// Wakes every 10ms, flushes accumulated mix data to hardware.

static void AudioMixerThreadFn(void* /*arg*/)
{
    SerialPuts("audio: mixer thread running\n");
    for (;;)
    {
        // Sleep ~10ms
        Process* self = ProcessCurrent();
        self->wakeupTick = g_lapicTickCount + 10;
        SchedulerBlock(self);

        // Flush whatever has accumulated
        if (g_mixerReady && g_audioDriver && g_audioDriver->play)
            AudioMixerTick();
    }
}

void AudioMixerThreadStart()
{
    Process* thread = KernelThreadCreate("audio_mix", AudioMixerThreadFn, nullptr);
    if (!thread)
    {
        SerialPuts("audio: failed to create mixer thread\n");
        return;
    }
    SchedulerAddProcess(thread);
    SerialPrintf("audio: mixer thread started (pid %u)\n", thread->pid);
}

} // namespace brook
