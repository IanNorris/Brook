// i_osssound.c — DOOM sound + music module using OSS /dev/dsp
//
// Replaces i_sdlsound.c and i_sdlmusic.c for Brook OS.
//
// SFX:   Simple software mixer decoding DMX lumps (8-bit unsigned PCM).
// Music: MUS→MIDI conversion via mus2mid(), then MIDI sequencer driving
//        Nuked OPL3 emulator with GENMIDI instrument data from WAD.
//        All mixed together and written to /dev/dsp as 16-bit stereo PCM.
//
// OPL3 emulator: Nuked OPL3 by Alexey Khokholov (Nuke.YKT), GPL2.
// Voice allocation and GENMIDI handling ported from Chocolate Doom's
// i_oplmusic.c by Simon Howard, GPL2.
//
// No SDL dependency. No libsamplerate.

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
#include "memio.h"
#include "mus2mid.h"
#include "doomtype.h"
#include "opl3.h"

// OSS ioctl definitions (Linux)
#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define AFMT_S16_LE           0x00000010

#define NUM_CHANNELS  16
#define MIX_FREQ      11025   // DOOM's native sample rate
#define MIX_BUFFER    512     // samples per mix buffer (mono)

// ============================================================
// SFX engine
// ============================================================

typedef struct {
    const unsigned char *data;
    unsigned int length;
    unsigned int pos;
    int vol_left;
    int vol_right;
    sfxinfo_t *sfxinfo;
} channel_t;

static channel_t channels[NUM_CHANNELS];
static int dsp_fd = -1;
static boolean sound_initialized = false;
static boolean use_sfx_prefix;

typedef struct {
    unsigned char *samples;
    unsigned int length;
    int ref_count;
} cached_sound_t;

static cached_sound_t *ExpandSound(const byte *data, int samplerate,
                                    unsigned int length)
{
    unsigned int expanded_len;
    cached_sound_t *cached;
    unsigned int i;

    if (samplerate == MIX_FREQ)
        expanded_len = length;
    else
        expanded_len = (unsigned int)(((uint64_t)length * MIX_FREQ) / samplerate);

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

static int I_OSS_GetSfxLumpNum(sfxinfo_t *sfx);

static boolean CacheSFX(sfxinfo_t *sfxinfo)
{
    int lumpnum;
    unsigned int lumplen;
    int samplerate;
    unsigned int length;
    byte *data;

    if (sfxinfo->driver_data) return true;

    lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0)
    {
        lumpnum = I_OSS_GetSfxLumpNum(sfxinfo);
        if (lumpnum < 0) return false;
        sfxinfo->lumpnum = lumpnum;
    }

    data = W_CacheLumpNum(lumpnum, PU_STATIC);
    lumplen = W_LumpLength(lumpnum);

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

    data += 16;
    length -= 32;

    sfxinfo->driver_data = ExpandSound(data + 8, samplerate, length);

    W_ReleaseLumpNum(lumpnum);
    return sfxinfo->driver_data != NULL;
}

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    if (use_sfx_prefix)
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    else
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
}

static int I_OSS_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[16];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

// ============================================================
// OPL3 Music Engine — Nuked OPL3 + GENMIDI from WAD
// ============================================================

// Byte-swap macro (no-op on little-endian x86)
#define SHORT(x) ((int16_t)(x))

// GENMIDI lump structures (packed, from Chocolate Doom)

typedef struct __attribute__((packed)) {
    uint8_t tremolo;
    uint8_t attack;
    uint8_t sustain;
    uint8_t waveform;
    uint8_t scale;
    uint8_t level;
} genmidi_op_t;

typedef struct __attribute__((packed)) {
    genmidi_op_t modulator;
    uint8_t feedback;
    genmidi_op_t carrier;
    uint8_t unused;
    int16_t base_note_offset;
} genmidi_voice_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint8_t fine_tuning;
    uint8_t fixed_note;
    genmidi_voice_t voices[2];
} genmidi_instr_t;

#define GENMIDI_NUM_INSTRS   128
#define GENMIDI_NUM_PERCUSSION 47
#define GENMIDI_HEADER       "#OPL_II#"
#define GENMIDI_FLAG_FIXED   0x0001
#define GENMIDI_FLAG_2VOICE  0x0004

// OPL register offsets
#define OPL_NUM_VOICES       9
#define OPL_REGS_TREMOLO     0x20
#define OPL_REGS_LEVEL       0x40
#define OPL_REGS_ATTACK      0x60
#define OPL_REGS_SUSTAIN     0x80
#define OPL_REGS_WAVEFORM    0xE0
#define OPL_REGS_FREQ_1      0xA0
#define OPL_REGS_FREQ_2      0xB0
#define OPL_REGS_FEEDBACK    0xC0

// Channel data (per MIDI channel)
typedef struct {
    genmidi_instr_t *instrument;
    int volume;
    int volume_base;
    int pan;
    int bend;
} opl_channel_data_t;

// OPL voice data
typedef struct {
    int index;
    int op1, op2;
    int array;
    genmidi_instr_t *current_instr;
    unsigned int current_instr_voice;
    opl_channel_data_t *channel;
    unsigned int key;
    unsigned int note;
    unsigned int freq;
    unsigned int note_volume;
    unsigned int car_volume;
    unsigned int mod_volume;
    unsigned int reg_pan;
    unsigned int priority;
} opl_voice_t;

// Operator mappings for the 9 OPL2 voices
static const int voice_operators[2][OPL_NUM_VOICES] = {
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 }
};

// Frequency values to use for each note (from Chocolate Doom)
static const unsigned short frequency_curve[] = {
    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,
    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,
    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,
    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,
    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,
    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,
    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,
    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,
    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,
    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,
    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,
    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,
    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,
    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,
    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,
    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,
    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,
    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,
    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,
    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,
    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

// MIDI volume to OPL level mapping (from Chocolate Doom)
static const unsigned int volume_mapping_table[] = {
    0, 1, 3, 5, 6, 8, 10, 11,
    13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34,
    36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61,
    63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81,
    82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

// Global OPL3 state
static opl3_chip opl_chip;
static genmidi_instr_t *main_instrs;
static genmidi_instr_t *percussion_instrs;

static opl_voice_t opl_voices[OPL_NUM_VOICES];
static opl_voice_t *voice_free_list[OPL_NUM_VOICES];
static opl_voice_t *voice_alloced_list[OPL_NUM_VOICES];
static int voice_free_num;
static int voice_alloced_num;

static opl_channel_data_t opl_channels[16];
static int music_volume = 127;

// Helper: write OPL register
static void OplWrite(int reg, int value)
{
    OPL3_WriteReg(&opl_chip, (Bit16u)reg, (Bit8u)value);
}

// Get the next available voice from the freelist
static opl_voice_t *OplGetFreeVoice(void)
{
    opl_voice_t *result;
    int i;

    if (voice_free_num == 0)
        return NULL;

    result = voice_free_list[0];
    voice_free_num--;
    for (i = 0; i < voice_free_num; i++)
        voice_free_list[i] = voice_free_list[i + 1];

    voice_alloced_list[voice_alloced_num++] = result;
    return result;
}

// Key off a voice (clear key-on bit in frequency register)
static void OplVoiceKeyOff(opl_voice_t *voice)
{
    OplWrite(OPL_REGS_FREQ_2 + voice->index, voice->freq >> 8);
}

// Release a voice back to the freelist
static void OplReleaseVoice(int index)
{
    opl_voice_t *voice;
    int i;

    if (index >= voice_alloced_num)
    {
        voice_alloced_num = 0;
        voice_free_num = 0;
        return;
    }

    voice = voice_alloced_list[index];
    OplVoiceKeyOff(voice);
    voice->channel = NULL;
    voice->note = 0;

    voice_alloced_num--;
    for (i = index; i < voice_alloced_num; i++)
        voice_alloced_list[i] = voice_alloced_list[i + 1];

    voice_free_list[voice_free_num++] = voice;
}

// When all voices are in use, steal one (Doom 1.9 behavior)
static void OplReplaceExistingVoice(void)
{
    int i;
    int result = 0;

    for (i = 0; i < voice_alloced_num; i++)
    {
        if (voice_alloced_list[i]->current_instr_voice != 0
         || voice_alloced_list[i]->channel
            >= voice_alloced_list[result]->channel)
        {
            result = i;
        }
    }

    OplReleaseVoice(result);
}

// Load data to the specified OPL operator
static void OplLoadOperatorData(int op, genmidi_op_t *data,
                                boolean max_level, unsigned int *volume)
{
    int level = data->scale;

    if (max_level)
        level |= 0x3f;
    else
        level |= data->level;

    *volume = level;

    OplWrite(OPL_REGS_LEVEL + op, level);
    OplWrite(OPL_REGS_TREMOLO + op, data->tremolo);
    OplWrite(OPL_REGS_ATTACK + op, data->attack);
    OplWrite(OPL_REGS_SUSTAIN + op, data->sustain);
    OplWrite(OPL_REGS_WAVEFORM + op, data->waveform);
}

// Set instrument for a voice
static void OplSetVoiceInstrument(opl_voice_t *voice,
                                  genmidi_instr_t *instr,
                                  unsigned int instr_voice)
{
    genmidi_voice_t *data;
    unsigned int modulating;

    if (voice->current_instr == instr
     && voice->current_instr_voice == instr_voice)
        return;

    voice->current_instr = instr;
    voice->current_instr_voice = instr_voice;

    data = &instr->voices[instr_voice];
    modulating = (data->feedback & 0x01) == 0;

    OplLoadOperatorData(voice->op2, &data->carrier, true, &voice->car_volume);
    OplLoadOperatorData(voice->op1, &data->modulator, !modulating, &voice->mod_volume);

    OplWrite(OPL_REGS_FEEDBACK + voice->index, data->feedback | voice->reg_pan);

    voice->priority = 0x0f - (data->carrier.attack >> 4)
                    + 0x0f - (data->carrier.sustain & 0x0f);
}

// Set volume for a voice
static void OplSetVoiceVolume(opl_voice_t *voice, unsigned int volume)
{
    genmidi_voice_t *opl_voice;
    unsigned int midi_volume;
    unsigned int full_volume;
    unsigned int car_volume;
    unsigned int mod_volume;

    voice->note_volume = volume;
    opl_voice = &voice->current_instr->voices[voice->current_instr_voice];

    midi_volume = 2 * (volume_mapping_table[voice->channel->volume] + 1);
    full_volume = (volume_mapping_table[voice->note_volume] * midi_volume) >> 9;
    car_volume = 0x3f - full_volume;

    if (car_volume != (voice->car_volume & 0x3f))
    {
        voice->car_volume = car_volume | (voice->car_volume & 0xc0);
        OplWrite(OPL_REGS_LEVEL + voice->op2, voice->car_volume);

        if ((opl_voice->feedback & 0x01) != 0
         && opl_voice->modulator.level != 0x3f)
        {
            mod_volume = opl_voice->modulator.level;
            if (mod_volume < car_volume)
                mod_volume = car_volume;
            mod_volume |= voice->mod_volume & 0xc0;

            if (mod_volume != voice->mod_volume)
            {
                voice->mod_volume = mod_volume;
                OplWrite(OPL_REGS_LEVEL + voice->op1,
                         mod_volume | (opl_voice->modulator.scale & 0xc0));
            }
        }
    }
}

// Calculate OPL frequency for a voice
static unsigned int OplFrequencyForVoice(opl_voice_t *voice)
{
    genmidi_voice_t *gm_voice;
    signed int freq_index;
    unsigned int octave;
    unsigned int sub_index;
    signed int note;

    note = voice->note;
    gm_voice = &voice->current_instr->voices[voice->current_instr_voice];

    if ((SHORT(voice->current_instr->flags) & GENMIDI_FLAG_FIXED) == 0)
        note += (signed short)SHORT(gm_voice->base_note_offset);

    while (note < 0) note += 12;
    while (note > 95) note -= 12;

    freq_index = 64 + 32 * note + voice->channel->bend;

    if (voice->current_instr_voice != 0)
        freq_index += (voice->current_instr->fine_tuning / 2) - 64;

    if (freq_index < 0) freq_index = 0;

    if (freq_index < 284)
        return frequency_curve[freq_index];

    sub_index = (freq_index - 284) % (12 * 32);
    octave = (freq_index - 284) / (12 * 32);

    if (octave >= 7) octave = 7;

    return frequency_curve[sub_index + 284] | (octave << 10);
}

// Update voice frequency registers
static void OplUpdateVoiceFrequency(opl_voice_t *voice)
{
    unsigned int freq = OplFrequencyForVoice(voice);

    if (voice->freq != freq)
    {
        OplWrite(OPL_REGS_FREQ_1 + voice->index, freq & 0xff);
        OplWrite(OPL_REGS_FREQ_2 + voice->index, (freq >> 8) | 0x20);
        voice->freq = freq;
    }
}

// Program and key-on a single voice
static void OplVoiceKeyOn(opl_channel_data_t *channel,
                          genmidi_instr_t *instrument,
                          unsigned int instrument_voice,
                          unsigned int note,
                          unsigned int key,
                          unsigned int volume)
{
    opl_voice_t *voice = OplGetFreeVoice();
    if (voice == NULL) return;

    voice->channel = channel;
    voice->key = key;

    if ((SHORT(instrument->flags) & GENMIDI_FLAG_FIXED) != 0)
        voice->note = instrument->fixed_note;
    else
        voice->note = note;

    voice->reg_pan = channel->pan;

    OplSetVoiceInstrument(voice, instrument, instrument_voice);
    OplSetVoiceVolume(voice, volume);

    voice->freq = 0;
    OplUpdateVoiceFrequency(voice);
}

// Handle Note On event
static void OplKeyOnEvent(int midi_ch, int note, int velocity)
{
    genmidi_instr_t *instrument;
    opl_channel_data_t *channel;
    boolean double_voice;

    if (velocity <= 0) {
        // Note off via zero velocity
        for (int i = 0; i < voice_alloced_num; i++) {
            if (voice_alloced_list[i]->channel == &opl_channels[midi_ch]
             && voice_alloced_list[i]->key == (unsigned int)note) {
                OplReleaseVoice(i);
                i--;
            }
        }
        return;
    }

    channel = &opl_channels[midi_ch];

    // Percussion channel (MIDI channel 9)
    if (midi_ch == 9) {
        if (note < 35 || note > 81) return;
        instrument = &percussion_instrs[note - 35];
        note = 60;
    } else {
        instrument = channel->instrument;
    }

    double_voice = (SHORT(instrument->flags) & GENMIDI_FLAG_2VOICE) != 0;

    // Doom 1.9 voice allocation
    if (voice_free_num == 0)
        OplReplaceExistingVoice();

    OplVoiceKeyOn(channel, instrument, 0, note, note, velocity);

    if (double_voice)
        OplVoiceKeyOn(channel, instrument, 1, note, note, velocity);
}

// Handle Note Off event
static void OplKeyOffEvent(int midi_ch, int note)
{
    for (int i = 0; i < voice_alloced_num; i++) {
        if (voice_alloced_list[i]->channel == &opl_channels[midi_ch]
         && voice_alloced_list[i]->key == (unsigned int)note) {
            OplReleaseVoice(i);
            i--;
        }
    }
}

// Handle Program Change
static void OplProgramChange(int midi_ch, int program)
{
    opl_channels[midi_ch].instrument = &main_instrs[program];
}

// Set channel volume and update all voices
static void OplSetChannelVolume(opl_channel_data_t *channel, unsigned int volume)
{
    unsigned int i;

    channel->volume_base = volume;
    if (volume > (unsigned int)music_volume)
        volume = music_volume;
    channel->volume = volume;

    for (i = 0; i < OPL_NUM_VOICES; ++i)
        if (opl_voices[i].channel == channel)
            OplSetVoiceVolume(&opl_voices[i], opl_voices[i].note_volume);
}

// Handle All Notes Off for a channel
static void OplAllNotesOff(int midi_ch)
{
    for (int i = 0; i < voice_alloced_num; i++) {
        if (voice_alloced_list[i]->channel == &opl_channels[midi_ch]) {
            OplReleaseVoice(i);
            i--;
        }
    }
}

// Handle Pitch Bend
static void OplPitchBend(int midi_ch, int bend_value)
{
    opl_channel_data_t *channel = &opl_channels[midi_ch];
    // Only MSB considered (Doom behavior)
    channel->bend = bend_value - 64;

    for (int i = 0; i < voice_alloced_num; ++i)
        if (voice_alloced_list[i]->channel == channel)
            OplUpdateVoiceFrequency(voice_alloced_list[i]);
}

// Initialize OPL channel to defaults
static void OplInitChannel(opl_channel_data_t *channel)
{
    channel->instrument = &main_instrs[0];
    channel->volume = music_volume;
    channel->volume_base = 100;
    if (channel->volume > channel->volume_base)
        channel->volume = channel->volume_base;
    channel->pan = 0x30;
    channel->bend = 0;
}

// Initialize OPL voices
static void OplInitVoices(void)
{
    int i;

    voice_free_num = OPL_NUM_VOICES;
    voice_alloced_num = 0;

    for (i = 0; i < OPL_NUM_VOICES; ++i) {
        opl_voices[i].index = i;
        opl_voices[i].op1 = voice_operators[0][i];
        opl_voices[i].op2 = voice_operators[1][i];
        opl_voices[i].array = 0;
        opl_voices[i].current_instr = NULL;
        voice_free_list[i] = &opl_voices[i];
    }
}

// Initialize OPL3 registers
static void OplInitRegisters(void)
{
    int r;

    for (r = 0x01; r <= 0xf5; ++r)
        OplWrite(r, 0x00);

    // Enable waveform selection
    OplWrite(0x01, 0x20);
}

// Load GENMIDI from WAD
static boolean OplLoadGenmidi(void)
{
    int lumpnum;
    byte *lump;

    lumpnum = W_CheckNumForName(DEH_String("GENMIDI"));
    if (lumpnum < 0) {
        fprintf(stderr, "OPL: GENMIDI lump not found\n");
        return false;
    }

    lump = W_CacheLumpNum(lumpnum, PU_STATIC);

    // Validate header
    if (memcmp(lump, GENMIDI_HEADER, 8) != 0) {
        fprintf(stderr, "OPL: Invalid GENMIDI header\n");
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    main_instrs = (genmidi_instr_t *)(lump + 8);
    percussion_instrs = main_instrs + GENMIDI_NUM_INSTRS;

    return true;
}

// ============================================================
// MIDI Sequencer
// ============================================================

typedef struct {
    unsigned char *data;
    size_t length;
    size_t track_start;
    size_t track_end;
    size_t pos;
    unsigned int ticks_per_qn;
    unsigned int tempo;
    unsigned int tick_accum;
    unsigned int samples_per_tick_q16;
    int playing;
    int looping;
    unsigned int generation;
} midi_state_t;

static midi_state_t midi_state;

static unsigned int MidiReadVarLen(midi_state_t *m)
{
    unsigned int val = 0;
    if (m->pos >= m->track_end) return 0;

    unsigned char c;
    do {
        if (m->pos >= m->track_end) break;
        c = m->data[m->pos++];
        val = (val << 7) | (c & 0x7F);
    } while (c & 0x80);

    return val;
}

static void MidiRecalcTempo(midi_state_t *m)
{
    if (m->ticks_per_qn == 0) m->ticks_per_qn = 70;
    m->samples_per_tick_q16 = (unsigned int)(
        (uint64_t)m->tempo * MIX_FREQ * 65536 / ((uint64_t)1000000 * m->ticks_per_qn)
    );
    if (m->samples_per_tick_q16 == 0) m->samples_per_tick_q16 = 1;
}

static boolean MidiParseSong(midi_state_t *m, unsigned char *data, size_t len)
{
    static unsigned int s_gen = 0;
    memset(m, 0, sizeof(*m));
    m->generation = ++s_gen;
    m->data = data;
    m->length = len;
    m->tempo = 500000;

    if (len < 14 || memcmp(data, "MThd", 4) != 0) return false;

    unsigned int header_len = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    m->ticks_per_qn = (data[12] << 8) | data[13];

    size_t offset = 8 + header_len;
    if (offset + 8 > len) return false;
    if (memcmp(data + offset, "MTrk", 4) != 0) return false;

    unsigned int track_len = (data[offset+4] << 24) | (data[offset+5] << 16)
                           | (data[offset+6] << 8)  | data[offset+7];

    m->track_start = offset + 8;
    m->track_end = m->track_start + track_len;
    if (m->track_end > len) m->track_end = len;
    m->pos = m->track_start;

    MidiRecalcTempo(m);
    return true;
}

// Process MIDI events up to the current time — now drives OPL3
static void MidiAdvance(midi_state_t *m, unsigned int samples)
{
    if (!m->playing) return;

    m->tick_accum += ((uint64_t)samples << 32) /
        (m->samples_per_tick_q16 > 0 ? m->samples_per_tick_q16 : 1);

    static unsigned char running_status = 0;
    static unsigned int next_delay = 0;
    static int delay_initialized = 0;
    static unsigned int song_generation = 0;

    if (m->generation != song_generation) {
        song_generation = m->generation;
        delay_initialized = 0;
        running_status = 0;
        next_delay = 0;
    }

    if (!delay_initialized) {
        next_delay = MidiReadVarLen(m);
        delay_initialized = 1;
    }

    int events_processed = 0;
    while (m->tick_accum >= ((uint32_t)next_delay << 16)) {
        m->tick_accum -= ((uint32_t)next_delay << 16);

        if (++events_processed > 200) break;

        if (m->pos >= m->track_end) {
            if (m->looping) {
                m->pos = m->track_start;
                delay_initialized = 0;
                running_status = 0;
                // Release all OPL voices for clean loop
                for (int ch = 0; ch < 16; ch++)
                    OplAllNotesOff(ch);
                next_delay = MidiReadVarLen(m);
                delay_initialized = 1;
                continue;
            }
            m->playing = 0;
            return;
        }

        // Read event
        unsigned char status = m->data[m->pos];
        if (status & 0x80) {
            m->pos++;
            running_status = status;
        } else {
            status = running_status;
        }

        int type = status & 0xF0;
        int ch = status & 0x0F;

        switch (type) {
        case 0x80: { // Note Off
            int note = m->data[m->pos++] & 0x7F;
            m->pos++; // velocity (ignored)
            OplKeyOffEvent(ch, note);
            break;
        }
        case 0x90: { // Note On
            int note = m->data[m->pos++] & 0x7F;
            int vel = m->data[m->pos++] & 0x7F;
            if (vel == 0)
                OplKeyOffEvent(ch, note);
            else
                OplKeyOnEvent(ch, note, vel);
            break;
        }
        case 0xA0: // Aftertouch
            m->pos += 2;
            break;
        case 0xB0: { // Control Change
            int cc = m->data[m->pos++] & 0x7F;
            int val = m->data[m->pos++] & 0x7F;
            switch (cc) {
                case 7:  // Volume
                    OplSetChannelVolume(&opl_channels[ch], val);
                    break;
                case 10: // Pan (ignored in OPL2 mode)
                    break;
                case 11: // Expression (treat like volume)
                    break;
                case 120: // All Sound Off
                case 123: // All Notes Off
                    OplAllNotesOff(ch);
                    break;
            }
            break;
        }
        case 0xC0: { // Program Change
            int prog = m->data[m->pos++] & 0x7F;
            OplProgramChange(ch, prog);
            break;
        }
        case 0xD0: // Channel Pressure
            m->pos++;
            break;
        case 0xE0: { // Pitch Bend
            int lsb = m->data[m->pos++];
            int msb = m->data[m->pos++];
            (void)lsb; // Only MSB used (Doom behavior)
            OplPitchBend(ch, msb);
            break;
        }
        case 0xF0: {
            if (status == 0xFF) { // Meta event
                int meta_type = m->data[m->pos++];
                unsigned int meta_len = MidiReadVarLen(m);
                if (meta_type == 0x51 && meta_len == 3) { // Tempo
                    m->tempo = (m->data[m->pos] << 16)
                             | (m->data[m->pos+1] << 8)
                             | m->data[m->pos+2];
                    MidiRecalcTempo(m);
                }
                if (meta_type == 0x2F) { // End of Track
                    if (m->looping) {
                        m->pos = m->track_start;
                        delay_initialized = 0;
                        running_status = 0;
                        for (int c = 0; c < 16; c++)
                            OplAllNotesOff(c);
                        next_delay = MidiReadVarLen(m);
                        delay_initialized = 1;
                        continue;
                    }
                    m->playing = 0;
                    return;
                }
                m->pos += meta_len;
            } else if (status == 0xF0 || status == 0xF7) { // SysEx
                unsigned int sysex_len = MidiReadVarLen(m);
                m->pos += sysex_len;
            }
            break;
        }
        }

        // Bounds check
        if (m->pos >= m->track_end) {
            if (m->looping) {
                m->pos = m->track_start;
                delay_initialized = 0;
                running_status = 0;
                for (int c = 0; c < 16; c++)
                    OplAllNotesOff(c);
                next_delay = MidiReadVarLen(m);
                delay_initialized = 1;
                continue;
            }
            m->playing = 0;
            return;
        }

        // Read next delay
        next_delay = MidiReadVarLen(m);
    }
}

// ============================================================
// Combined mixer (SFX + OPL Music → /dev/dsp)
// ============================================================

static void MixAndWrite(void)
{
    int32_t mixbuf[MIX_BUFFER * 2];
    memset(mixbuf, 0, sizeof(mixbuf));

    // Mix SFX channels
    for (int c = 0; c < NUM_CHANNELS; c++) {
        channel_t *ch = &channels[c];
        if (!ch->data) continue;

        for (int i = 0; i < MIX_BUFFER; i++) {
            if (ch->pos >= ch->length) {
                if (ch->sfxinfo && ch->sfxinfo->driver_data) {
                    cached_sound_t *cached = ch->sfxinfo->driver_data;
                    cached->ref_count--;
                }
                ch->data = NULL;
                ch->sfxinfo = NULL;
                break;
            }

            int sample = ((int)ch->data[ch->pos] - 128) * 256;
            ch->pos++;

            mixbuf[i * 2]     += (sample * ch->vol_left) / 255;
            mixbuf[i * 2 + 1] += (sample * ch->vol_right) / 255;
        }
    }

    // Render OPL3 music and add to mix buffer
    if (midi_state.playing) {
        MidiAdvance(&midi_state, MIX_BUFFER);

        int16_t opl_buf[MIX_BUFFER * 2];
        OPL3_GenerateStream(&opl_chip, opl_buf, MIX_BUFFER);

        for (int i = 0; i < MIX_BUFFER * 2; i++)
            mixbuf[i] += opl_buf[i];
    }

    // Clamp to int16 and write
    int16_t outbuf[MIX_BUFFER * 2];
    for (int i = 0; i < MIX_BUFFER * 2; i++) {
        int val = mixbuf[i];
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        outbuf[i] = (int16_t)val;
    }

    if (dsp_fd >= 0)
        write(dsp_fd, outbuf, sizeof(outbuf));
}

// ============================================================
// SFX module interface
// ============================================================

static int I_OSS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    cached_sound_t *cached;

    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS)
        return -1;

    if (channels[channel].data && channels[channel].sfxinfo) {
        cached_sound_t *old = channels[channel].sfxinfo->driver_data;
        if (old) old->ref_count--;
    }

    if (!CacheSFX(sfxinfo))
        return -1;

    cached = sfxinfo->driver_data;
    cached->ref_count++;

    channels[channel].data = cached->samples;
    channels[channel].length = cached->length;
    channels[channel].pos = 0;
    channels[channel].sfxinfo = sfxinfo;

    int left_frac  = 254 - sep;
    int right_frac = sep;

    channels[channel].vol_left  = (vol * left_frac) / 127;
    channels[channel].vol_right = (vol * right_frac) / 127;

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

    channels[handle].vol_left  = (vol * left_frac) / 127;
    channels[handle].vol_right = (vol * right_frac) / 127;

    if (channels[handle].vol_left > 255)  channels[handle].vol_left = 255;
    if (channels[handle].vol_right > 255) channels[handle].vol_right = 255;
}

static void I_OSS_UpdateSound(void)
{
    if (!sound_initialized) return;
    MixAndWrite();
}

static void I_OSS_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    for (int i = 0; i < num_sounds; i++)
        CacheSFX(&sounds[i]);
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

    for (i = 0; i < NUM_CHANNELS; i++)
        memset(&channels[i], 0, sizeof(channel_t));

    memset(&midi_state, 0, sizeof(midi_state));

    dsp_fd = open("/dev/dsp", O_WRONLY);
    if (dsp_fd < 0) {
        fprintf(stderr, "I_OSS_InitSound: cannot open /dev/dsp\n");
        return false;
    }

    val = AFMT_S16_LE;
    ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &val);

    val = 2;
    ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &val);

    val = MIX_FREQ;
    ioctl(dsp_fd, SNDCTL_DSP_SPEED, &val);

    fprintf(stderr, "I_OSS_InitSound: /dev/dsp opened, %d Hz stereo 16-bit\n", MIX_FREQ);

    sound_initialized = true;
    return true;
}

// ============================================================
// Music module interface
// ============================================================

static boolean music_initialized = false;

typedef struct {
    unsigned char *midi_data;
    size_t midi_len;
} registered_song_t;

static boolean I_OSS_InitMusic(void)
{
    // Initialize OPL3 emulator
    OPL3_Reset(&opl_chip, MIX_FREQ);

    // Initialize OPL registers
    OplInitRegisters();

    // Load GENMIDI from WAD
    if (!OplLoadGenmidi()) {
        fprintf(stderr, "I_OSS_InitMusic: Failed to load GENMIDI\n");
        return false;
    }

    OplInitVoices();

    for (int i = 0; i < 16; i++)
        OplInitChannel(&opl_channels[i]);

    music_initialized = true;
    fprintf(stderr, "I_OSS_InitMusic: OPL3 music enabled (Nuked OPL3 + GENMIDI)\n");
    return true;
}

static void I_OSS_ShutdownMusic(void)
{
    midi_state.playing = 0;
    music_initialized = false;
}

static void I_OSS_SetMusicVolume(int volume)
{
    unsigned int i;

    if (music_volume == volume)
        return;

    music_volume = volume;

    // Update all channel volumes
    for (i = 0; i < 16; ++i)
        OplSetChannelVolume(&opl_channels[i], opl_channels[i].volume_base);
}

static void I_OSS_PauseMusic(void)
{
    midi_state.playing = 0;
}

static void I_OSS_ResumeMusic(void)
{
    if (midi_state.data)
        midi_state.playing = 1;
}

static void *I_OSS_RegisterSong(void *data, int len)
{
    if (!music_initialized) return NULL;

    registered_song_t *song = malloc(sizeof(registered_song_t));
    if (!song) return NULL;

    // Check if it's already MIDI
    if (len > 4 && memcmp(data, "MThd", 4) == 0) {
        song->midi_data = malloc(len);
        if (!song->midi_data) { free(song); return NULL; }
        memcpy(song->midi_data, data, len);
        song->midi_len = len;
    }
    // Check if it's MUS format
    else if (len > 4 && memcmp(data, "MUS\x1a", 4) == 0) {
        MEMFILE *instream = mem_fopen_read(data, len);
        MEMFILE *outstream = mem_fopen_write();

        if (mus2mid(instream, outstream)) {
            mem_fclose(instream);
            mem_fclose(outstream);
            free(song);
            return NULL;
        }

        void *outbuf;
        size_t outbuf_len;
        mem_get_buf(outstream, &outbuf, &outbuf_len);

        song->midi_data = malloc(outbuf_len);
        if (!song->midi_data) {
            mem_fclose(instream);
            mem_fclose(outstream);
            free(song);
            return NULL;
        }
        memcpy(song->midi_data, outbuf, outbuf_len);
        song->midi_len = outbuf_len;

        mem_fclose(instream);
        mem_fclose(outstream);
    } else {
        free(song);
        return NULL;
    }

    fprintf(stderr, "I_OSS_RegisterSong: registered %zu byte MIDI\n", song->midi_len);
    return song;
}

static void I_OSS_UnRegisterSong(void *handle)
{
    registered_song_t *song = handle;
    if (!song) return;

    if (midi_state.data == song->midi_data) {
        midi_state.playing = 0;
        midi_state.data = NULL;
    }

    free(song->midi_data);
    free(song);
}

static void I_OSS_PlaySong(void *handle, boolean looping)
{
    registered_song_t *song = handle;
    if (!song || !song->midi_data) return;

    // Stop any current playback
    midi_state.playing = 0;

    // Release all OPL voices
    for (int ch = 0; ch < 16; ch++)
        OplAllNotesOff(ch);

    // Re-initialize OPL registers for a clean start
    OplInitRegisters();
    OplInitVoices();

    // Reset channel state
    for (int i = 0; i < 16; i++)
        OplInitChannel(&opl_channels[i]);

    if (!MidiParseSong(&midi_state, song->midi_data, song->midi_len)) {
        fprintf(stderr, "I_OSS_PlaySong: failed to parse MIDI\n");
        return;
    }

    midi_state.looping = looping;
    midi_state.playing = 1;

    fprintf(stderr, "I_OSS_PlaySong: playing (%s), %u ticks/qn, tempo %u us/qn\n",
            looping ? "looping" : "once",
            midi_state.ticks_per_qn, midi_state.tempo);
}

static void I_OSS_StopSong(void)
{
    midi_state.playing = 0;
    for (int ch = 0; ch < 16; ch++)
        OplAllNotesOff(ch);
}

static boolean I_OSS_MusicIsPlaying(void)
{
    return midi_state.playing;
}

static void I_OSS_PollMusic(void)
{
    // Music is rendered inline in MixAndWrite(), nothing extra needed
}

// ============================================================
// Module exports
// ============================================================

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
    SNDDEVICE_SB,
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
