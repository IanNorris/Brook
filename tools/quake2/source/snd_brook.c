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
static int submit_pos = 0;  /* mono samples written so far */

#define SND_BUF_SAMPLES  16384  /* total mono samples in ring buffer */
static unsigned char dma_buffer[SND_BUF_SAMPLES * 2 * 2]; /* 16-bit stereo */

/* Pending SFX buffer: filled by SNDDMA_Submit, consumed by SFX_MixInto.
 * Holds at most one frame of 11025 Hz stereo int16 audio (~460 frames = 1840 bytes). */
#define SFX_PENDING_BYTES  4096
static unsigned char g_sfx_pending[SFX_PENDING_BYTES];
int g_sfx_pending_bytes = 0;

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
    submit_pos = 0;
    snd_inited = 1;

    Com_Printf("SNDDMA_Init: /dev/dsp opened, %d Hz, 16-bit stereo\n", dma.speed);
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    if (!snd_inited)
        return 0;

    dma.samplepos = submit_pos & (dma.samples - 1);
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

void SNDDMA_Submit(void)
{
    if (!snd_inited)
        return;

    extern int paintedtime;
    int new_samples = (paintedtime - (submit_pos / dma.channels)) * dma.channels;
    if (new_samples <= 0)
    {
        g_sfx_pending_bytes = 0;
        return;
    }

    /* Cap at one frame's worth at 11025 Hz to avoid getting too far ahead */
    int max_samples = (11025 / 25 + 16) * dma.channels;
    if (new_samples > max_samples)
        new_samples = max_samples;

    int sample_bytes = dma.samplebits / 8;
    int buf_len = dma.samples * sample_bytes;
    int pos = (submit_pos * sample_bytes) & (buf_len - 1);
    int total_bytes = new_samples * sample_bytes;
    if (total_bytes > SFX_PENDING_BYTES)
        total_bytes = SFX_PENDING_BYTES;

    /* Copy from ring buffer (handling wrap) into pending buffer for cd_brook.c */
    int chunk1 = buf_len - pos;
    if (chunk1 > total_bytes) chunk1 = total_bytes;
    memcpy(g_sfx_pending, dma_buffer + pos, chunk1);
    if (chunk1 < total_bytes)
        memcpy(g_sfx_pending + chunk1, dma_buffer, total_bytes - chunk1);

    g_sfx_pending_bytes = total_bytes;
    submit_pos += new_samples;
}

/*
 * SFX_MixInto — called by cd_brook.c to blend pending SFX into the CD audio
 * buffer before writing to /dev/dsp.  Resamples from 11025 Hz to 44100 Hz
 * (nearest-neighbour, 4× upsample) and adds with int16 clamping.
 * Clears the pending buffer on return.
 */
int SFX_MixInto(short* dst, int dst_frames)
{
    if (g_sfx_pending_bytes <= 0)
        return 0;

    int src_frames = g_sfx_pending_bytes / 4; /* 4 bytes = 1 stereo int16 frame */
    short* src = (short*)g_sfx_pending;

    for (int i = 0; i < dst_frames; i++)
    {
        int si = (int)((long long)i * src_frames / dst_frames);
        if (si >= src_frames) si = src_frames - 1;

        int l = (int)dst[i*2]   + (int)src[si*2];
        int r = (int)dst[i*2+1] + (int)src[si*2+1];
        dst[i*2]   = l >  32767 ?  32767 : l < -32768 ? -32768 : (short)l;
        dst[i*2+1] = r >  32767 ?  32767 : r < -32768 ? -32768 : (short)r;
    }

    g_sfx_pending_bytes = 0;
    return dst_frames;
}
