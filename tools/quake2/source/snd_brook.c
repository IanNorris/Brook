/*
 * snd_brook.c — OSS /dev/dsp sound backend for Quake 2 on Brook OS
 *
 * Write-based model: Quake 2 mixes into a local ring buffer,
 * we write newly mixed samples to /dev/dsp on each Submit() call.
 * The kernel resamples from 11025Hz to 44100Hz.
 */

#include "client.h"
#include "snd_loc.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* OSS ioctl definitions (Brook kernel values) */
#define SNDCTL_DSP_RESET      0x5000
#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define AFMT_S16_LE           0x00000010

static int audio_fd = -1;
static int snd_inited = 0;
static int snd_start_ms = 0;

#define SND_BUF_SAMPLES  16384  /* total mono samples in ring buffer */
static unsigned char dma_buffer[SND_BUF_SAMPLES * 2 * 2]; /* 16-bit stereo */

qboolean SNDDMA_Init(void)
{
    audio_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (audio_fd < 0)
    {
        Com_Printf("SNDDMA_Init: can't open /dev/dsp\n");
        return false;
    }

    int val;

    /* 16-bit signed LE */
    val = AFMT_S16_LE;
    ioctl(audio_fd, SNDCTL_DSP_SETFMT, &val);

    /* stereo */
    val = 2;
    ioctl(audio_fd, SNDCTL_DSP_CHANNELS, &val);

    /* 11025 Hz — kernel resamples to 44100 */
    val = 11025;
    ioctl(audio_fd, SNDCTL_DSP_SPEED, &val);

    /* Set up the DMA descriptor */
    dma.samplebits = 16;
    dma.speed = 11025;
    dma.channels = 2;
    dma.samples = SND_BUF_SAMPLES;
    dma.samplepos = 0;
    dma.submission_chunk = 1;
    dma.buffer = dma_buffer;

    memset(dma_buffer, 0, sizeof(dma_buffer));
    snd_start_ms = Sys_Milliseconds();
    snd_inited = 1;

    Com_Printf("SNDDMA_Init: /dev/dsp opened, %d Hz, 16-bit stereo\n", dma.speed);
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    if (!snd_inited)
        return 0;

    /* Return a position that advances at exactly dma.speed * dma.channels int16
     * values per second in real time.  GetSoundtime() divides by dma.channels,
     * so soundtime advances at dma.speed pairs/sec — correct for 11025 Hz. */
    int elapsed_ms = Sys_Milliseconds() - snd_start_ms;
    int total = (int)((long long)elapsed_ms * dma.speed * dma.channels / 1000);
    dma.samplepos = total & (dma.samples - 1);
    return dma.samplepos;
}

void SNDDMA_Shutdown(void)
{
    if (audio_fd >= 0)
    {
        close(audio_fd);
        audio_fd = -1;
    }
    snd_inited = 0;
}

void SNDDMA_BeginPainting(void)
{
}

/* SNDDMA_Submit is a no-op: SFX audio is read directly from dma_buffer by
 * SFX_MixInto using paintedtime.  Q2 calls this after each mix cycle but
 * we don't need to do anything here. */
void SNDDMA_Submit(void)
{
}

/*
 * SFX_MixInto — blend this frame's SFX into dst (44100 Hz stereo int16).
 *
 * Reads exactly dst_frames * dma.speed / 44100 frames directly from
 * dma_buffer ending at paintedtime — always correct pitch regardless of
 * framerate.  Uses nearest-neighbour upsample (dma.speed → 44100 Hz).
 */
int SFX_MixInto(short* dst, int dst_frames)
{
    if (!snd_inited || !dma.buffer || dst_frames <= 0)
        return 0;

    extern int paintedtime;

    /* Compute how many dma.speed-Hz stereo frames correspond to dst_frames
     * at 44100 Hz.  dma.speed/44100 = 11025/44100 = 1/4. */
    int src_frames = dst_frames * dma.speed / 44100;
    if (src_frames <= 0)
        return 0;

    /* dma_buffer is a ring of (dma.samples / dma.channels) stereo frames.
     * paintedtime is the mix cursor in stereo frames.
     * The most recently mixed audio is in [paintedtime - src_frames, paintedtime). */
    int stereo_ring = dma.samples >> 1;              /* 8192 stereo frames */
    int bytes_per_frame = (dma.samplebits / 8) * dma.channels; /* 4 */
    int start = paintedtime - src_frames;

    for (int i = 0; i < dst_frames; i++)
    {
        int si = (int)((long long)i * src_frames / dst_frames);
        int frame = (start + si) & (stereo_ring - 1);
        short* src = (short*)(dma_buffer + frame * bytes_per_frame);

        int l = (int)dst[i*2]   + (int)src[0];
        int r = (int)dst[i*2+1] + (int)src[1];
        dst[i*2]   = l >  32767 ?  32767 : l < -32768 ? -32768 : (short)l;
        dst[i*2+1] = r >  32767 ?  32767 : r < -32768 ? -32768 : (short)r;
    }

    return dst_frames;
}
