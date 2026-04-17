// i_osssound.c — DOOM sound module using OSS /dev/dsp
//
// Replaces i_sdlsound.c for Brook OS. Implements a simple software mixer
// that decodes DOOM's DMX sound lumps (8-bit unsigned PCM) and writes
// mixed 16-bit stereo PCM to /dev/dsp via standard OSS ioctls.
//
// No SDL dependency. No libsamplerate. Just raw PCM mixing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "config.h"
#include "deh_str.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doomtype.h"

// OSS ioctl definitions (Linux)
#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define AFMT_S16_LE           0x00000010

#define NUM_CHANNELS  16
#define MIX_FREQ      11025   // DOOM's native sample rate
#define MIX_BUFFER    512     // samples per mix buffer (mono)

// Per-channel state
typedef struct {
    const unsigned char *data;  // 8-bit unsigned PCM samples
    unsigned int length;        // total samples
    unsigned int pos;           // current playback position
    int vol_left;               // 0-255 left volume
    int vol_right;              // 0-255 right volume
    sfxinfo_t *sfxinfo;         // which sound effect
} channel_t;

static channel_t channels[NUM_CHANNELS];
static int dsp_fd = -1;
static boolean sound_initialized = false;
static boolean use_sfx_prefix;

// Expanded (resampled) sound cache stored in driver_data
typedef struct {
    unsigned char *samples;  // 8-bit unsigned PCM at MIX_FREQ
    unsigned int length;
    int ref_count;
} cached_sound_t;

// Simple nearest-neighbor resample from source rate to MIX_FREQ
static cached_sound_t *ExpandSound(const byte *data, int samplerate,
                                    unsigned int length)
{
    unsigned int expanded_len;
    cached_sound_t *cached;
    unsigned int i;

    if (samplerate == MIX_FREQ) {
        expanded_len = length;
    } else {
        expanded_len = (unsigned int)(((uint64_t)length * MIX_FREQ) / samplerate);
    }

    if (expanded_len == 0) expanded_len = 1;

    cached = malloc(sizeof(cached_sound_t));
    if (!cached) return NULL;

    cached->samples = malloc(expanded_len);
    if (!cached->samples) { free(cached); return NULL; }

    cached->length = expanded_len;
    cached->ref_count = 0;

    if (samplerate == MIX_FREQ) {
        memcpy(cached->samples, data, length);
    } else {
        for (i = 0; i < expanded_len; i++) {
            unsigned int src_pos = (unsigned int)(((uint64_t)i * samplerate) / MIX_FREQ);
            if (src_pos >= length) src_pos = length - 1;
            cached->samples[i] = data[src_pos];
        }
    }

    return cached;
}

static boolean CacheSFX(sfxinfo_t *sfxinfo)
{
    int lumpnum;
    unsigned int lumplen;
    int samplerate;
    unsigned int length;
    byte *data;

    if (sfxinfo->driver_data) return true;

    lumpnum = sfxinfo->lumpnum;
    data = W_CacheLumpNum(lumpnum, PU_STATIC);
    lumplen = W_LumpLength(lumpnum);

    // DMX header: 0x0003, uint16 samplerate, uint32 length
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    samplerate = (data[3] << 8) | data[2];
    length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];

    if (length > lumplen - 8 || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    // DMX skips first 16 and last 16 bytes
    data += 16;
    length -= 32;

    sfxinfo->driver_data = ExpandSound(data + 8, samplerate, length);

    W_ReleaseLumpNum(lumpnum);
    return sfxinfo->driver_data != NULL;
}

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    if (use_sfx_prefix) {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    } else {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static int I_OSS_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[16];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void MixAndWrite(void)
{
    int16_t mixbuf[MIX_BUFFER * 2]; // stereo
    int i, c;

    memset(mixbuf, 0, sizeof(mixbuf));

    for (c = 0; c < NUM_CHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->data) continue;

        for (i = 0; i < MIX_BUFFER; i++) {
            if (ch->pos >= ch->length) {
                // Sound finished
                if (ch->sfxinfo && ch->sfxinfo->driver_data) {
                    cached_sound_t *cached = ch->sfxinfo->driver_data;
                    cached->ref_count--;
                }
                ch->data = NULL;
                ch->sfxinfo = NULL;
                break;
            }

            // Convert 8-bit unsigned to signed 16-bit: (sample - 128) * 256
            int sample = ((int)ch->data[ch->pos] - 128) * 256;
            ch->pos++;

            // Apply per-channel volume and stereo panning
            int left  = (sample * ch->vol_left) / 255;
            int right = (sample * ch->vol_right) / 255;

            // Mix (clamp later)
            int mixed_l = mixbuf[i * 2]     + left;
            int mixed_r = mixbuf[i * 2 + 1] + right;

            // Clamp to int16 range
            if (mixed_l > 32767)  mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767)  mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;

            mixbuf[i * 2]     = (int16_t)mixed_l;
            mixbuf[i * 2 + 1] = (int16_t)mixed_r;
        }
    }

    if (dsp_fd >= 0) {
        write(dsp_fd, mixbuf, sizeof(mixbuf));
    }
}

static int I_OSS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    cached_sound_t *cached;

    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS)
        return -1;

    // Stop whatever was playing on this channel
    if (channels[channel].data && channels[channel].sfxinfo) {
        cached_sound_t *old = channels[channel].sfxinfo->driver_data;
        if (old) old->ref_count--;
    }

    // Cache the sound if needed
    if (!CacheSFX(sfxinfo))
        return -1;

    cached = sfxinfo->driver_data;
    cached->ref_count++;

    channels[channel].data = cached->samples;
    channels[channel].length = cached->length;
    channels[channel].pos = 0;
    channels[channel].sfxinfo = sfxinfo;

    // sep: 0=full left, 127=center, 254=full right
    // vol: 0-127
    int left_frac  = 254 - sep;  // 0..254
    int right_frac = sep;         // 0..254

    channels[channel].vol_left  = (vol * left_frac) / (127 * 127);
    channels[channel].vol_right = (vol * right_frac) / (127 * 127);

    // Scale to 0..255
    if (channels[channel].vol_left > 255)  channels[channel].vol_left = 255;
    if (channels[channel].vol_right > 255) channels[channel].vol_right = 255;

    return channel;
}

static void I_OSS_StopSound(int handle)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_CHANNELS)
        return;

    if (channels[handle].sfxinfo && channels[handle].sfxinfo->driver_data) {
        cached_sound_t *cached = channels[handle].sfxinfo->driver_data;
        cached->ref_count--;
    }

    channels[handle].data = NULL;
    channels[handle].sfxinfo = NULL;
}

static boolean I_OSS_SoundIsPlaying(int handle)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_CHANNELS)
        return false;
    return channels[handle].data != NULL;
}

static void I_OSS_UpdateSoundParams(int handle, int vol, int sep)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_CHANNELS)
        return;

    int left_frac  = 254 - sep;
    int right_frac = sep;

    channels[handle].vol_left  = (vol * left_frac) / (127 * 127);
    channels[handle].vol_right = (vol * right_frac) / (127 * 127);

    if (channels[handle].vol_left > 255)  channels[handle].vol_left = 255;
    if (channels[handle].vol_right > 255) channels[handle].vol_right = 255;
}

static void I_OSS_UpdateSound(void)
{
    if (!sound_initialized) return;

    // Mix all active channels and write to /dev/dsp
    MixAndWrite();
}

static void I_OSS_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    int i;
    for (i = 0; i < num_sounds; i++) {
        CacheSFX(&sounds[i]);
    }
}

static void I_OSS_ShutdownSound(void)
{
    if (!sound_initialized) return;

    if (dsp_fd >= 0) {
        close(dsp_fd);
        dsp_fd = -1;
    }
    sound_initialized = false;
}

static boolean I_OSS_InitSound(boolean _use_sfx_prefix)
{
    int i, val;

    use_sfx_prefix = _use_sfx_prefix;

    for (i = 0; i < NUM_CHANNELS; i++) {
        memset(&channels[i], 0, sizeof(channel_t));
    }

    dsp_fd = open("/dev/dsp", O_WRONLY);
    if (dsp_fd < 0) {
        fprintf(stderr, "I_OSS_InitSound: cannot open /dev/dsp\n");
        return false;
    }

    // Configure: 16-bit signed LE, stereo, 11025 Hz (DOOM native rate)
    val = AFMT_S16_LE;
    ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &val);

    val = 2;
    ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &val);

    val = MIX_FREQ;
    ioctl(dsp_fd, SNDCTL_DSP_SPEED, &val);

    fprintf(stderr, "I_OSS_InitSound: /dev/dsp opened, %d Hz stereo 16-bit\n",
            MIX_FREQ);

    sound_initialized = true;
    return true;
}

// Music stubs — no music support yet (needs MIDI/MUS decoder)

static boolean I_OSS_InitMusic(void) { return true; }
static void I_OSS_ShutdownMusic(void) {}
static void I_OSS_SetMusicVolume(int volume) { (void)volume; }
static void I_OSS_PauseMusic(void) {}
static void I_OSS_ResumeMusic(void) {}
static void *I_OSS_RegisterSong(void *data, int len) {
    (void)data; (void)len; return NULL;
}
static void I_OSS_UnRegisterSong(void *handle) { (void)handle; }
static void I_OSS_PlaySong(void *handle, boolean looping) {
    (void)handle; (void)looping;
}
static void I_OSS_StopSong(void) {}
static boolean I_OSS_MusicIsPlaying(void) { return false; }
static void I_OSS_PollMusic(void) {}

// Module exports

static snddevice_t sound_oss_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    sound_oss_devices,
    sizeof(sound_oss_devices) / sizeof(*sound_oss_devices),
    I_OSS_InitSound,
    I_OSS_ShutdownSound,
    I_OSS_GetSfxLumpNum,
    I_OSS_UpdateSound,
    I_OSS_UpdateSoundParams,
    I_OSS_StartSound,
    I_OSS_StopSound,
    I_OSS_SoundIsPlaying,
    I_OSS_PrecacheSounds,
};

static snddevice_t music_oss_devices[] = {
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module = {
    music_oss_devices,
    sizeof(music_oss_devices) / sizeof(*music_oss_devices),
    I_OSS_InitMusic,
    I_OSS_ShutdownMusic,
    I_OSS_SetMusicVolume,
    I_OSS_PauseMusic,
    I_OSS_ResumeMusic,
    I_OSS_RegisterSong,
    I_OSS_UnRegisterSong,
    I_OSS_PlaySong,
    I_OSS_StopSong,
    I_OSS_MusicIsPlaying,
    I_OSS_PollMusic,
};

int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
