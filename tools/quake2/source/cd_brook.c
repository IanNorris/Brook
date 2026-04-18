/*
 * cd_brook.c — OGG Vorbis CD-audio replacement for Quake 2 on Brook OS.
 *
 * Replaces the null CD driver.  When the engine calls CDAudio_Play(track),
 * we open baseq2/music/TrackNN.ogg, decode with stb_vorbis, and stream
 * 16-bit stereo 44100 Hz PCM to a second /dev/dsp file descriptor.
 *
 * The kernel mixes all /dev/dsp writers together, so this coexists with
 * the SFX output in snd_brook.c.
 */

#include "client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ---- stb_vorbis (header-only mode) ---- */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO          /* we load files ourselves */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

/* OSS ioctls (must match kernel values) */
#define SNDCTL_DSP_RESET     0x5000
#define SNDCTL_DSP_SPEED     0xC0045002
#define SNDCTL_DSP_SETFMT    0xC0045005
#define SNDCTL_DSP_CHANNELS  0xC0045006
#define AFMT_S16_LE          0x00000010

/* ---- state ---- */
static int            cd_fd      = -1;   /* /dev/dsp fd for music */
static stb_vorbis    *cd_vorbis  = NULL;
static unsigned char *cd_filedata = NULL;
static int            cd_filelen = 0;
static qboolean       cd_looping = false;
static qboolean       cd_playing = false;
static int            cd_track   = 0;

/* Max decode buffer: 2 seconds of audio at 44100 Hz stereo (safety cap) */
#define CD_FRAMES_PER_CHUNK  4096
static short cd_pcm[CD_FRAMES_PER_CHUNK * 2]; /* stereo interleaved */

/* ---- helpers ---- */

static void cd_close_track(void)
{
    if (cd_vorbis) { stb_vorbis_close(cd_vorbis); cd_vorbis = NULL; }
    if (cd_filedata) { free(cd_filedata); cd_filedata = NULL; }
    cd_filelen = 0;
    cd_playing = false;
}

/* Load an entire file into malloc'd memory.  Returns length or -1. */
static int cd_load_file(const char *path, unsigned char **out)
{
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;

    /* Seek to end to get size */
    int len = (int)lseek(f, 0, SEEK_END);
    if (len <= 0) { close(f); return -1; }
    lseek(f, 0, SEEK_SET);

    *out = (unsigned char *)malloc(len);
    if (!*out) { close(f); return -1; }

    int total = 0;
    while (total < len)
    {
        int r = read(f, *out + total, len - total);
        if (r <= 0) break;
        total += r;
    }
    close(f);
    return total;
}

static qboolean cd_open_track(int track)
{
    char path[256];
    int n;

    cd_close_track();

    /* Try basedir/baseq2/music/TrackNN.ogg */
    n = snprintf(path, sizeof(path), "%s/baseq2/music/Track%02d.ogg",
                 Cvar_VariableString("basedir"), track);
    if (n < 0 || n >= (int)sizeof(path)) return false;

    cd_filelen = cd_load_file(path, &cd_filedata);
    if (cd_filelen <= 0)
    {
        Com_Printf("CDAudio: can't load %s\n", path);
        return false;
    }

    int vorbis_error = 0;
    cd_vorbis = stb_vorbis_open_memory(cd_filedata, cd_filelen, &vorbis_error, NULL);
    if (!cd_vorbis)
    {
        Com_Printf("CDAudio: vorbis decode error %d for %s\n", vorbis_error, path);
        free(cd_filedata); cd_filedata = NULL; cd_filelen = 0;
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(cd_vorbis);
    Com_Printf("CDAudio: playing track %d (%d Hz, %d ch, %d bytes)\n",
               track, info.sample_rate, info.channels, cd_filelen);

    cd_track = track;
    cd_playing = true;
    return true;
}

/* ---- public API ---- */

int CDAudio_Init(void)
{
    cd_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (cd_fd < 0)
    {
        Com_Printf("CDAudio_Init: can't open /dev/dsp for music\n");
        return -1;
    }

    int val;
    val = AFMT_S16_LE;   ioctl(cd_fd, SNDCTL_DSP_SETFMT, &val);
    val = 2;             ioctl(cd_fd, SNDCTL_DSP_CHANNELS, &val);
    val = 44100;         ioctl(cd_fd, SNDCTL_DSP_SPEED, &val);

    Com_Printf("CDAudio_Init: music via /dev/dsp, 44100 Hz stereo\n");
    return 0;
}

void CDAudio_Shutdown(void)
{
    cd_close_track();
    if (cd_fd >= 0) { close(cd_fd); cd_fd = -1; }
}

void CDAudio_Play(int track, qboolean looping)
{
    if (cd_fd < 0) return;

    /* Track 0/1 = no music */
    if (track <= 1)
    {
        CDAudio_Stop();
        return;
    }

    cd_looping = looping;
    if (!cd_open_track(track))
        CDAudio_Stop();
}

void CDAudio_Stop(void)
{
    cd_close_track();
}

void CDAudio_Activate(qboolean active)
{
    /* nothing — we don't pause/resume */
}

void CDAudio_Update(void)
{
    if (!cd_playing || !cd_vorbis || cd_fd < 0)
        return;

    /* Decode exactly as many frames as elapsed real time warrants.
     * cls.frametime is seconds since last frame; at 44100 Hz this gives
     * the exact number of output frames the hardware will consume this frame.
     * Clamped to [1, CD_FRAMES_PER_CHUNK] to handle large/tiny frametimes. */
    int want = (int)(44100.0f * cls.frametime);
    if (want < 1)   want = 1;
    if (want > CD_FRAMES_PER_CHUNK) want = CD_FRAMES_PER_CHUNK;

    int frames = stb_vorbis_get_samples_short_interleaved(
                     cd_vorbis, 2, cd_pcm, want * 2);

    if (frames <= 0)
    {
        /* End of track */
        if (cd_looping)
        {
            stb_vorbis_seek_start(cd_vorbis);
            frames = stb_vorbis_get_samples_short_interleaved(
                         cd_vorbis, 2, cd_pcm, want * 2);
            if (frames <= 0) { cd_close_track(); return; }
        }
        else
        {
            cd_close_track();
            return;
        }
    }

    int bytes = frames * 2 * sizeof(short); /* stereo 16-bit */
    write(cd_fd, cd_pcm, bytes);  /* O_NONBLOCK — don't stall game loop */
}
