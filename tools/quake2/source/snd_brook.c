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
    // SFX temporarily disabled — testing CD music path only
    if (!snd_inited)
        return;

    // Advance submit_pos so Quake's mixer doesn't stall thinking
    // the buffer is full, but don't actually write anything.
    extern int paintedtime;
    int new_samples = (paintedtime - (submit_pos / dma.channels)) * dma.channels;
    if (new_samples > 0)
        submit_pos += new_samples;
}
