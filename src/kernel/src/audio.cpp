// audio.cpp — Brook audio subsystem registry + software mixer.
//
// The mixer is a ring buffer into which /dev/dsp writers submit resampled
// 44100 Hz stereo 16-bit PCM.  Multiple streams are additively mixed.
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

// --- Mixer ring buffer ---
// Ring of int32 stereo samples.  Writers add into it at their stream's
// write cursor; the tick drains from readPos to the hw-write high-water mark.
//
// Layout: [readPos ... writeHighWater ... end] wrapping around.
// Multiple streams can write concurrently at different positions; we drain
// up to the minimum write position to ensure all streams have contributed.

static constexpr uint32_t MIXER_BUF_FRAMES = 16384; // ~372ms at 44100 Hz
static constexpr uint32_t MIXER_BUF_MASK   = MIXER_BUF_FRAMES - 1;
static constexpr uint32_t MIXER_MAX_STREAMS = 8;

// Flush target: drain up to this many frames per tick (~10ms = 441 frames).
// We target slightly larger chunks to reduce per-call overhead.
static constexpr uint32_t MIXER_TICK_FRAMES = 882; // ~20ms chunk

static int32_t* g_mixBuf = nullptr;     // [MIXER_BUF_FRAMES * 2] (L/R interleaved)
static uint32_t g_readPos = 0;          // next frame to be flushed to hardware
static uint32_t g_streamWritePos[MIXER_MAX_STREAMS]; // per-stream write cursor (absolute)
static uint32_t g_writeHighWater = 0;   // max of all stream write positions
static bool     g_mixerReady = false;
static int16_t* g_flushBuf = nullptr;   // temporary int16 buffer for flush

void AudioMixerInit()
{
    g_mixBuf = static_cast<int32_t*>(kmalloc(MIXER_BUF_FRAMES * 2 * sizeof(int32_t)));
    g_flushBuf = static_cast<int16_t*>(kmalloc(MIXER_TICK_FRAMES * 2 * 2 * sizeof(int16_t)));
    if (g_mixBuf && g_flushBuf)
    {
        memset(g_mixBuf, 0, MIXER_BUF_FRAMES * 2 * sizeof(int32_t));
        g_readPos = 0;
        g_writeHighWater = 0;
        memset(g_streamWritePos, 0, sizeof(g_streamWritePos));
        g_mixerReady = true;
    }
}

// How many frames are buffered (available to flush).
static inline uint32_t MixerBufferedFrames()
{
    return g_writeHighWater - g_readPos;
}

uint32_t AudioMixerAvailableFrames(uint32_t streamId)
{
    if (!g_mixerReady) return 0;
    if (streamId >= MIXER_MAX_STREAMS) streamId = 0;

    SpinLockAcquire(&g_mixerLock);
    uint32_t writePos = g_streamWritePos[streamId];
    if (writePos < g_readPos)
        writePos = g_readPos;
    uint32_t available = MIXER_BUF_FRAMES - (writePos - g_readPos);
    SpinLockRelease(&g_mixerLock);
    return available;
}

void AudioMixerSubmit(const int16_t* samples, uint32_t frameCount, uint32_t streamId)
{
    if (!g_mixerReady || !samples || frameCount == 0) return;
    if (streamId >= MIXER_MAX_STREAMS) streamId = 0;

    SpinLockAcquire(&g_mixerLock);

    uint32_t writePos = g_streamWritePos[streamId];

    // If stream is behind readPos (just started or was idle), catch up
    if (writePos < g_readPos)
        writePos = g_readPos;

    // If this write would overflow the ring, clamp to available space
    uint32_t available = MIXER_BUF_FRAMES - (writePos - g_readPos);
    if (frameCount > available)
        frameCount = available;

    if (frameCount == 0)
    {
        SpinLockRelease(&g_mixerLock);
        return;
    }

    // Additively mix into the ring buffer
    for (uint32_t i = 0; i < frameCount; i++)
    {
        uint32_t ringIdx = (writePos + i) & MIXER_BUF_MASK;
        g_mixBuf[ringIdx * 2 + 0] += static_cast<int32_t>(samples[i * 2 + 0]);
        g_mixBuf[ringIdx * 2 + 1] += static_cast<int32_t>(samples[i * 2 + 1]);
    }

    writePos += frameCount;
    g_streamWritePos[streamId] = writePos;
    if (writePos > g_writeHighWater)
        g_writeHighWater = writePos;

    SpinLockRelease(&g_mixerLock);
}

// Flush accumulated mix data to hardware.
// Lock is acquired/released internally — play() may block.

void AudioMixerFlush()
{
    SpinLockAcquire(&g_mixerLock);

    if (!g_mixerReady || !g_audioDriver || !g_audioDriver->play)
    {
        SpinLockRelease(&g_mixerLock);
        return;
    }

    uint32_t buffered = MixerBufferedFrames();
    if (buffered == 0)
    {
        SpinLockRelease(&g_mixerLock);
        return;
    }

    // Flush up to 2x tick size per call (avoid huge bursts)
    uint32_t toFlush = buffered;
    if (toFlush > MIXER_TICK_FRAMES * 2)
        toFlush = MIXER_TICK_FRAMES * 2;

    // Convert int32 → int16 with clamping into the flush buffer
    for (uint32_t i = 0; i < toFlush; i++)
    {
        uint32_t ringIdx = (g_readPos + i) & MIXER_BUF_MASK;
        int32_t l = g_mixBuf[ringIdx * 2 + 0];
        int32_t r = g_mixBuf[ringIdx * 2 + 1];
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        g_flushBuf[i * 2 + 0] = static_cast<int16_t>(l);
        g_flushBuf[i * 2 + 1] = static_cast<int16_t>(r);
        g_mixBuf[ringIdx * 2 + 0] = 0;
        g_mixBuf[ringIdx * 2 + 1] = 0;
    }

    g_readPos += toFlush;

    // Release lock BEFORE calling into the HDA driver — play() may block
    // (SchedulerSleepMs) when the HDA ring buffer is full. Holding a
    // spinlock across a sleep would stall all AudioMixerSubmit callers.
    SpinLockRelease(&g_mixerLock);

    uint32_t byteCount = toFlush * MIXER_FRAME_BYTES;
    g_audioDriver->play(g_flushBuf, byteCount,
                        MIXER_HW_RATE, MIXER_HW_CHANNELS, MIXER_HW_BITS, false);
}

void AudioMixerTick()
{
    if (!g_mixerReady) return;
    AudioMixerFlush();
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
    if (g_audioDriver && g_audioDriver->stop)
        g_audioDriver->stop();
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
            AudioMixerFlush();
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
