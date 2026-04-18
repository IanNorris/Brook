// audio.cpp — Brook audio subsystem registry + software mixer.
//
// The mixer accumulates 44100 Hz stereo 16-bit PCM from multiple /dev/dsp
// writers, adds samples together with clamping, and flushes to the hardware
// driver in chunks.

#include "audio.h"
#include "serial.h"
#include "kprintf.h"
#include "memory/heap.h"

namespace brook {

static const AudioDriver* g_audioDriver = nullptr;

// --- Mixer state ---
// Mix buffer holds accumulated frames (stereo int32 to avoid clipping during add).
// Flushed to hardware as 16-bit PCM.
// Each /dev/dsp fd has its own write cursor (stream ID) so multiple streams
// accumulate at the same time positions and are properly added together.
static constexpr uint32_t MIXER_BUF_FRAMES = 8192; // ~186ms at 44100 Hz
static constexpr uint32_t MIXER_MAX_STREAMS = 8;

static int32_t* g_mixBuf = nullptr;     // [MIXER_BUF_FRAMES * 2] (L/R interleaved)
static uint32_t g_mixFrames = 0;        // high-water mark of frames written
static uint32_t g_mixStreamPos[MIXER_MAX_STREAMS]; // per-stream write cursor
static bool     g_mixerReady = false;

void AudioMixerInit()
{
    g_mixBuf = static_cast<int32_t*>(kmalloc(MIXER_BUF_FRAMES * 2 * sizeof(int32_t)));
    if (g_mixBuf)
    {
        for (uint32_t i = 0; i < MIXER_BUF_FRAMES * 2; i++)
            g_mixBuf[i] = 0;
        g_mixFrames = 0;
        for (uint32_t i = 0; i < MIXER_MAX_STREAMS; i++)
            g_mixStreamPos[i] = 0;
        g_mixerReady = true;
    }
}

void AudioMixerSubmit(const int16_t* samples, uint32_t frameCount, uint32_t streamId)
{
    if (!g_mixerReady || !samples || frameCount == 0) return;
    if (streamId >= MIXER_MAX_STREAMS) streamId = 0;

    uint32_t writePos = g_mixStreamPos[streamId];

    // If this write won't fit, flush everything first
    if (writePos + frameCount > MIXER_BUF_FRAMES)
    {
        AudioMixerFlush();
        writePos = 0;
    }

    // Clamp to buffer
    if (frameCount > MIXER_BUF_FRAMES)
        frameCount = MIXER_BUF_FRAMES;

    // Add samples into mix buffer (int32 accumulation avoids clipping)
    uint32_t sampleCount = frameCount * 2; // stereo
    uint32_t offset = writePos * 2;
    for (uint32_t i = 0; i < sampleCount; i++)
        g_mixBuf[offset + i] += static_cast<int32_t>(samples[i]);

    writePos += frameCount;
    g_mixStreamPos[streamId] = writePos;
    if (writePos > g_mixFrames)
        g_mixFrames = writePos;
}

void AudioMixerFlush()
{
    if (!g_mixerReady || !g_audioDriver || !g_audioDriver->play || g_mixFrames == 0)
        return;

    // Convert int32 mix buffer → int16 with clamping
    uint32_t sampleCount = g_mixFrames * 2;

    // Find the minimum stream position — that's how much ALL streams have written.
    // We can safely flush up to the min position since all streams have contributed.
    // However, for simplicity and to avoid stalls, flush everything up to g_mixFrames.
    // Pack int16 into the first half of the int32 buffer (safe since int16 < int32)
    int16_t* outBuf = reinterpret_cast<int16_t*>(g_mixBuf);
    for (uint32_t i = 0; i < sampleCount; i++)
    {
        int32_t s = g_mixBuf[i];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        outBuf[i] = static_cast<int16_t>(s);
    }

    uint32_t byteCount = g_mixFrames * MIXER_FRAME_BYTES;
    g_audioDriver->play(outBuf, byteCount, MIXER_HW_RATE, MIXER_HW_CHANNELS, MIXER_HW_BITS);

    // Clear the flushed region and reset stream positions
    uint32_t flushedFrames = g_mixFrames;
    for (uint32_t i = 0; i < flushedFrames * 2; i++)
        g_mixBuf[i] = 0;

    // Adjust stream positions: subtract flushed amount (they start fresh)
    for (uint32_t i = 0; i < MIXER_MAX_STREAMS; i++)
    {
        if (g_mixStreamPos[i] <= flushedFrames)
            g_mixStreamPos[i] = 0;
        else
            g_mixStreamPos[i] -= flushedFrames;
    }
    g_mixFrames = 0;
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
              uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample)
{
    if (!g_audioDriver || !g_audioDriver->play) return -1;
    return g_audioDriver->play(samples, byteCount, sampleRate, channels, bitsPerSample);
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

} // namespace brook
