/*
 * snd_oss.c — OSS /dev/dsp sound backend for Quake on Brook OS
 *
 * Implements SNDDMA_Init, SNDDMA_GetDMAPos, SNDDMA_Shutdown, SNDDMA_Submit.
 * Uses a write-based model: mixes into a local ring buffer, then writes
 * newly mixed samples to /dev/dsp on each Submit() call.
 */

#include "quakedef.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* OSS ioctl definitions (Brook kernel values) */
#define SNDCTL_DSP_RESET      0x5000
#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define AFMT_S16_LE           0x00000010

static int  audio_fd = -1;
static int  snd_inited = 0;

/* Number of samples submitted to /dev/dsp so far (in mono samples) */
static int  submit_pos = 0;

/* DMA buffer — Quake mixes into this, we write() to /dev/dsp */
#define SND_BUF_SAMPLES  16384  /* mono samples (= 8192 stereo frames) */
static unsigned char dma_buffer[SND_BUF_SAMPLES * 2 * 2]; /* 16-bit stereo worst case */

qboolean SNDDMA_Init(void)
{
    audio_fd = open("/dev/dsp", O_WRONLY);
    if (audio_fd < 0)
    {
        Con_Printf("SNDDMA_Init: can't open /dev/dsp\n");
        return false;
    }

    int val;

    /* 16-bit signed LE */
    val = AFMT_S16_LE;
    ioctl(audio_fd, SNDCTL_DSP_SETFMT, &val);

    /* stereo */
    val = 2;
    ioctl(audio_fd, SNDCTL_DSP_CHANNELS, &val);

    /* 11025 Hz — Quake's default; kernel resamples to 44100 */
    val = 11025;
    ioctl(audio_fd, SNDCTL_DSP_SPEED, &val);

    /* Set up the shared DMA descriptor */
    shm = &sn;
    shm->splitbuffer  = 0;
    shm->samplebits   = 16;
    shm->speed        = 11025;
    shm->channels     = 2;
    shm->samples      = SND_BUF_SAMPLES;  /* total mono samples in buffer */
    shm->samplepos    = 0;
    shm->submission_chunk = 1;
    shm->soundalive   = true;
    shm->gamealive    = true;
    shm->buffer       = dma_buffer;

    memset(dma_buffer, 0, sizeof(dma_buffer));
    submit_pos = 0;
    snd_inited = 1;

    Con_Printf("SNDDMA_Init: /dev/dsp opened, %d Hz, 16-bit stereo\n", shm->speed);
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    if (!snd_inited)
        return 0;

    /* Return our write position — Quake uses this to know how far audio
     * has been "consumed" so it can mix ahead of this point. We advance
     * submit_pos in SNDDMA_Submit after writing to /dev/dsp. */
    shm->samplepos = submit_pos & (shm->samples - 1);
    return shm->samplepos;
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

void SNDDMA_Submit(void)
{
    if (!snd_inited || audio_fd < 0)
        return;

    /* Calculate how many mono samples have been newly mixed since last submit.
     * paintedtime is advanced by S_PaintChannels; submit_pos tracks what
     * we've already written to /dev/dsp. */
    int new_samples = (paintedtime - (submit_pos / shm->channels)) * shm->channels;
    if (new_samples <= 0)
        return;

    int sample_bytes = shm->samplebits / 8;
    int buf_len = shm->samples * sample_bytes; /* total ring buffer bytes */
    int pos = (submit_pos * sample_bytes) & (buf_len - 1);
    int total_bytes = new_samples * sample_bytes;

    /* Write in up to two chunks (wrap around ring buffer) */
    while (total_bytes > 0)
    {
        int chunk = buf_len - pos;
        if (chunk > total_bytes)
            chunk = total_bytes;

        ssize_t w = write(audio_fd, dma_buffer + pos, chunk);
        if (w < 0)
            break;

        pos = (pos + w) & (buf_len - 1);
        total_bytes -= w;
        submit_pos += w / sample_bytes;
    }
}
