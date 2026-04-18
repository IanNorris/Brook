// i_osssound.c — DOOM sound + music module using OSS /dev/dsp
//
// Replaces i_sdlsound.c and i_sdlmusic.c for Brook OS.
//
// SFX:   Simple software mixer decoding DMX lumps (8-bit unsigned PCM).
// Music: MUS→MIDI conversion via mus2mid(), then a built-in MIDI sequencer
//        with 2-operator FM synthesis (OPL2-inspired). All mixed together
//        and written to /dev/dsp as 16-bit stereo PCM.
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

    // Resolve lump number if not yet set (lumpnum is -1 until first play)
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
// FM Synthesis engine — simplified 2-operator OPL2-style
// ============================================================

// 256-entry sine table (Q15 fixed-point, range -32767..32767)
static int16_t fm_sine[256];

static void FmInitSineTable(void)
{
    // Approximate sine via a polynomial or use a precomputed table.
    // sin(x) ≈ x - x³/6 + x⁵/120  (good enough for FM audio)
    for (int i = 0; i < 256; i++) {
        // angle in radians: i * 2π / 256
        // Use integer-only approximation:
        // Map i ∈ [0,256) to angle ∈ [0, 2π)
        // sin via symmetry: compute for quadrant 0, mirror
        int q = i & 63;  // position within quadrant (0..63)
        int quadrant = (i >> 6) & 3;

        // Linear interpolation within quadrant using:
        // sin(q * π/128) ≈ q * 32767 / 64 for small angles (Q1)
        // Better: use a proper lookup
        // We'll compute using the identity and fixed-point math

        // x = q * π / 128, compute sin(x) * 32767
        // Use Taylor: sin(x) = x - x³/6
        // x in Q15: x_q15 = q * 32767 * π / (128 * 1)
        // Simpler: precompute for one quadrant

        // sin(i * 2π / 256) scaled to Q15
        // Quadrant 0 (i=0..63): sin rises 0..1
        // Use: sin(q*π/128) with lookup for quadrant 0

        // Pre-calculate using integer math:
        // For q in [0..64], sin(q * π/128) * 32767
        // ≈ (q * 804) for small q, tapering off
        // Actually let's just hard-code a proper table generation:

        // x = q * 201 / 64 (≈ π * 64 scaled to ~201 in Q0)
        // Use Bhaskara's sine approximation: sin(x) ≈ 16x(π-x) / (5π²-4x(π-x))
        // where x ∈ [0, π]

        // For simplicity, compute in integer with enough precision:
        // angle_deg = i * 360 / 256
        // We'll use a different approach: direct quadrant computation

        // q ranges 0..63, mapping to 0..π/2
        // sin(q * π/128) = cos((64-q) * π/128)
        // Use: val = (q * (128 - q) * 40967) >> 15
        // This is Bhaskara's approximation adapted

        int p = q;
        if (quadrant == 1 || quadrant == 3) p = 64 - q;

        // Bhaskara I: sin(x) ≈ 16x(180-x) / [5*180² - 4x(180-x)]
        // Adapted for p ∈ [0,64] → angle ∈ [0,90°]:
        // sin(p * 90/64 degrees) using integer Bhaskara
        // Let a = p, range [0,64], maps to [0, 90°]
        // Bhaskara: sin(a°) ≈ 4a(180-a) / (40500 - a(180-a))
        // But our a is in [0,90], so a_deg = p * 90 / 64

        int a_deg = (p * 90 + 32) / 64;  // [0..90]
        int term = a_deg * (180 - a_deg); // [0..8100]
        int val;
        if (term == 0)
            val = 0;
        else
            val = (int)((int64_t)4 * term * 32767 / (40500 - term));

        if (val > 32767) val = 32767;

        if (quadrant >= 2) val = -val;

        fm_sine[i] = (int16_t)val;
    }
}

// Per-voice state for FM synthesis
#define FM_MAX_VOICES 32
#define FM_POLYPHONY  FM_MAX_VOICES

typedef struct {
    int active;
    int midi_channel;  // 0-15
    int note;          // MIDI note number (0-127)
    int velocity;      // 0-127
    uint32_t phase;    // carrier phase accumulator (Q24)
    uint32_t mod_phase; // modulator phase accumulator (Q24)
    uint32_t carrier_freq;  // carrier frequency in Q24 (increments per sample)
    uint32_t mod_freq;      // modulator frequency
    int mod_depth;     // modulation depth (0-255)
    int env_level;     // envelope level (0-255), simple decay
    int env_decay;     // decay rate per MIX_BUFFER (subtracted from env_level)
    int patch;         // MIDI program number
} fm_voice_t;

static fm_voice_t fm_voices[FM_MAX_VOICES];

// Per-MIDI-channel state
typedef struct {
    int program;    // current patch (0-127)
    int volume;     // channel volume (0-127), CC7
    int pan;        // pan (0=left, 64=center, 127=right), CC10
    int expression; // expression (0-127), CC11
    int pitch_bend; // pitch bend (-8192..8191)
} midi_channel_t;

static midi_channel_t midi_channels[16];

// MIDI note to frequency table (Q24 fixed-point, increments per sample at MIX_FREQ)
// freq = 440 * 2^((note-69)/12)
// phase_inc = freq * 256 / MIX_FREQ  (Q24 where 256 = one full cycle of fm_sine)
static uint32_t note_freq_table[128];

static void FmInitFreqTable(void)
{
    // Compute note_freq_table[n] = (440 * 2^((n-69)/12)) * 256 * 65536 / MIX_FREQ
    // in Q24 format (16.16 with 8 bits for sine table index)
    // = 440 * 256 * 65536 / MIX_FREQ * 2^((n-69)/12)

    // Base: note 69 (A4) = 440 Hz
    // phase_inc for 440 Hz = 440 * 256 / MIX_FREQ in Q16
    // = 440 * 256 * 65536 / MIX_FREQ

    // For MIX_FREQ=11025: 440 * 256 / 11025 = 10.216..
    // In Q16: 10.216 * 65536 = 669548

    // Use integer math with sufficient precision
    for (int n = 0; n < 128; n++) {
        // Compute 2^((n-69)/12) using integer approximation
        // Start from note 69 and multiply/divide by 2^(1/12) ≈ 1.05946
        // In Q16: 2^(1/12) ≈ 69433/65536

        int64_t base = (int64_t)440 * 256 * 65536 / MIX_FREQ;  // Q16 for A4
        int diff = n - 69;

        if (diff > 0) {
            for (int i = 0; i < diff; i++)
                base = base * 69433 / 65536;
        } else if (diff < 0) {
            for (int i = 0; i < -diff; i++)
                base = base * 65536 / 69433;
        }

        note_freq_table[n] = (uint32_t)base;
    }
}

// Patch definitions: modulator ratio and depth for different instrument families
// DOOM uses General MIDI patches. We map groups to FM parameters.
typedef struct {
    int mod_ratio_num;   // modulator freq = carrier * num / den
    int mod_ratio_den;
    int mod_depth;       // 0-255
    int decay_rate;      // envelope decay per buffer (0=sustain, higher=faster decay)
    int brightness;      // carrier volume scale (0-255)
} fm_patch_t;

// Simplified GM patch mapping — group instruments by family
static const fm_patch_t gm_patches[128] = {
    // 0-7: Piano
    [0]  = {2,1, 180, 2, 255}, [1]  = {2,1, 160, 2, 240}, [2]  = {2,1, 200, 3, 255},
    [3]  = {2,1, 140, 2, 220}, [4]  = {3,1, 160, 3, 230}, [5]  = {3,1, 180, 3, 240},
    [6]  = {2,1, 120, 2, 200}, [7]  = {1,1, 100, 2, 200},
    // 8-15: Chromatic Percussion
    [8]  = {4,1, 200, 8, 255}, [9]  = {3,1, 180, 6, 240}, [10] = {5,1, 220, 10, 255},
    [11] = {4,1, 160, 8, 230}, [12] = {6,1, 200, 12, 250}, [13] = {3,1, 140, 6, 220},
    [14] = {4,1, 180, 10, 240}, [15] = {5,1, 200, 12, 250},
    // 16-23: Organ
    [16] = {1,1, 80, 0, 220}, [17] = {1,1, 60, 0, 200}, [18] = {2,1, 100, 0, 230},
    [19] = {1,1, 120, 0, 240}, [20] = {3,1, 80, 0, 200}, [21] = {2,1, 60, 0, 200},
    [22] = {1,1, 40, 0, 180}, [23] = {2,1, 80, 0, 200},
    // 24-31: Guitar
    [24] = {2,1, 140, 4, 230}, [25] = {2,1, 120, 3, 220}, [26] = {3,1, 160, 4, 240},
    [27] = {2,1, 100, 3, 210}, [28] = {3,1, 180, 5, 250}, [29] = {3,1, 200, 5, 255},
    [30] = {4,1, 220, 6, 255}, [31] = {2,1, 160, 4, 230},
    // 32-39: Bass
    [32] = {1,1, 100, 3, 255}, [33] = {1,1, 80, 2, 240}, [34] = {1,1, 120, 3, 250},
    [35] = {2,1, 100, 3, 240}, [36] = {1,1, 60, 2, 230}, [37] = {2,1, 80, 2, 220},
    [38] = {1,1, 100, 3, 250}, [39] = {2,1, 120, 3, 255},
    // 40-47: Strings
    [40] = {1,1, 40, 0, 200}, [41] = {1,1, 30, 0, 190}, [42] = {1,1, 50, 0, 210},
    [43] = {1,1, 60, 1, 220}, [44] = {2,1, 40, 0, 200}, [45] = {1,1, 20, 0, 180},
    [46] = {2,1, 60, 1, 210}, [47] = {1,1, 80, 2, 220},
    // 48-55: Ensemble
    [48] = {1,1, 40, 0, 200}, [49] = {1,1, 30, 0, 190}, [50] = {1,1, 50, 0, 210},
    [51] = {1,1, 60, 0, 220}, [52] = {2,1, 30, 0, 180}, [53] = {1,1, 20, 0, 170},
    [54] = {1,1, 80, 1, 230}, [55] = {1,1, 100, 2, 240},
    // 56-63: Brass
    [56] = {1,1, 120, 1, 240}, [57] = {1,1, 100, 1, 230}, [58] = {1,1, 140, 1, 250},
    [59] = {2,1, 100, 1, 230}, [60] = {1,1, 80, 1, 220}, [61] = {2,1, 120, 1, 240},
    [62] = {1,1, 160, 2, 255}, [63] = {2,1, 140, 2, 250},
    // 64-71: Reed
    [64] = {2,1, 100, 1, 220}, [65] = {3,1, 120, 1, 230}, [66] = {2,1, 80, 1, 210},
    [67] = {3,1, 100, 1, 220}, [68] = {2,1, 60, 0, 200}, [69] = {3,1, 80, 1, 210},
    [70] = {2,1, 100, 1, 220}, [71] = {3,1, 120, 1, 230},
    // 72-79: Pipe
    [72] = {1,1, 20, 0, 180}, [73] = {1,1, 10, 0, 170}, [74] = {2,1, 30, 0, 180},
    [75] = {1,1, 40, 0, 190}, [76] = {2,1, 20, 0, 180}, [77] = {1,1, 60, 0, 200},
    [78] = {2,1, 40, 0, 190}, [79] = {1,1, 80, 1, 210},
    // 80-87: Synth Lead
    [80] = {2,1, 160, 1, 250}, [81] = {3,1, 180, 1, 255}, [82] = {1,1, 40, 0, 200},
    [83] = {2,1, 120, 1, 240}, [84] = {3,1, 140, 1, 240}, [85] = {1,1, 60, 0, 210},
    [86] = {2,1, 200, 2, 255}, [87] = {3,1, 220, 2, 255},
    // 88-95: Synth Pad
    [88] = {1,1, 30, 0, 190}, [89] = {1,1, 20, 0, 180}, [90] = {2,1, 40, 0, 200},
    [91] = {1,1, 50, 0, 210}, [92] = {2,1, 30, 0, 190}, [93] = {1,1, 40, 0, 200},
    [94] = {2,1, 60, 0, 210}, [95] = {1,1, 80, 1, 220},
    // 96-103: Synth Effects
    [96] = {4,1, 200, 6, 250}, [97] = {3,1, 160, 4, 240}, [98] = {5,1, 180, 8, 250},
    [99] = {2,1, 100, 2, 220}, [100]= {3,1, 120, 3, 230}, [101]= {4,1, 160, 5, 240},
    [102]= {5,1, 200, 8, 250}, [103]= {6,1, 220, 10, 255},
    // 104-111: Ethnic
    [104]= {3,1, 160, 4, 240}, [105]= {2,1, 120, 3, 230}, [106]= {4,1, 180, 5, 240},
    [107]= {3,1, 140, 4, 230}, [108]= {2,1, 100, 3, 220}, [109]= {3,1, 120, 4, 230},
    [110]= {2,1, 80, 2, 210}, [111]= {3,1, 100, 3, 220},
    // 112-119: Percussive
    [112]= {4,1, 220, 10, 255}, [113]= {5,1, 240, 12, 255}, [114]= {3,1, 180, 8, 250},
    [115]= {6,1, 200, 15, 250}, [116]= {4,1, 200, 10, 250}, [117]= {5,1, 220, 12, 255},
    [118]= {3,1, 160, 6, 240}, [119]= {4,1, 180, 8, 250},
    // 120-127: Sound Effects
    [120]= {1,1, 20, 0, 160}, [121]= {1,1, 10, 0, 140}, [122]= {6,1, 200, 20, 250},
    [123]= {1,1, 40, 0, 180}, [124]= {1,1, 60, 4, 200}, [125]= {1,1, 80, 8, 220},
    [126]= {1,1, 100, 12, 230}, [127]= {1,1, 0, 0, 100},
};

static fm_voice_t *FmAllocVoice(int midi_ch, int note)
{
    // First check if this note is already playing on this channel (retrigger)
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (fm_voices[i].active && fm_voices[i].midi_channel == midi_ch
            && fm_voices[i].note == note)
            return &fm_voices[i];
    }
    // Find a free voice
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (!fm_voices[i].active)
            return &fm_voices[i];
    }
    // Steal the quietest voice
    int quietest = 0;
    int min_level = fm_voices[0].env_level;
    for (int i = 1; i < FM_MAX_VOICES; i++) {
        if (fm_voices[i].env_level < min_level) {
            min_level = fm_voices[i].env_level;
            quietest = i;
        }
    }
    return &fm_voices[quietest];
}

static void FmNoteOn(int midi_ch, int note, int velocity)
{
    if (note > 127) return;
    fm_voice_t *v = FmAllocVoice(midi_ch, note);

    const fm_patch_t *p = &gm_patches[midi_channels[midi_ch].program];
    // Use defaults if patch has zero brightness (uninitialized)
    int mod_depth = p->brightness > 0 ? p->mod_depth : 120;
    int mod_ratio_num = p->brightness > 0 ? p->mod_ratio_num : 2;
    int mod_ratio_den = p->brightness > 0 ? p->mod_ratio_den : 1;
    int decay = p->brightness > 0 ? p->decay_rate : 2;

    v->active = 1;
    v->midi_channel = midi_ch;
    v->note = note;
    v->velocity = velocity;
    v->phase = 0;
    v->mod_phase = 0;
    v->carrier_freq = note_freq_table[note];
    v->mod_freq = (uint32_t)((uint64_t)note_freq_table[note] * mod_ratio_num / mod_ratio_den);
    v->mod_depth = mod_depth;
    v->env_level = 255;
    v->env_decay = decay;
    v->patch = midi_channels[midi_ch].program;
}

static void FmNoteOff(int midi_ch, int note)
{
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (fm_voices[i].active && fm_voices[i].midi_channel == midi_ch
            && fm_voices[i].note == note) {
            // Fast release: accelerate decay
            fm_voices[i].env_decay = fm_voices[i].env_decay < 8 ? 8 : fm_voices[i].env_decay;
        }
    }
}

// Render FM voices into a buffer (adds to existing content)
static void FmRender(int32_t *buf, int num_samples)
{
    for (int v = 0; v < FM_MAX_VOICES; v++) {
        fm_voice_t *voice = &fm_voices[v];
        if (!voice->active) continue;

        midi_channel_t *mch = &midi_channels[voice->midi_channel];
        int ch_vol = mch->volume > 0 ? mch->volume : 100;
        int ch_expr = mch->expression > 0 ? mch->expression : 127;
        int ch_pan = mch->pan;  // 0-127, 64=center

        // Overall voice volume: velocity * channel_vol * expression * envelope / (127³)
        // Simplified to avoid overflow:
        int vol = (voice->velocity * ch_vol * voice->env_level) / (127 * 127);
        vol = (vol * ch_expr) / 127;
        if (vol <= 0) { voice->active = 0; continue; }
        if (vol > 255) vol = 255;

        // Stereo panning
        int vol_l = vol * (127 - ch_pan) / 64;
        int vol_r = vol * ch_pan / 64;
        if (vol_l > 255) vol_l = 255;
        if (vol_r > 255) vol_r = 255;

        for (int i = 0; i < num_samples; i++) {
            // Modulator oscillator
            int mod_idx = (voice->mod_phase >> 16) & 0xFF;
            int mod_out = (fm_sine[mod_idx] * voice->mod_depth) >> 8;

            // Carrier oscillator with FM
            int carrier_phase = (int)(voice->phase >> 16) + (mod_out >> 7);
            int carrier_idx = carrier_phase & 0xFF;
            int sample = fm_sine[carrier_idx];

            // Apply volume
            int left  = (sample * vol_l) >> 10;
            int right = (sample * vol_r) >> 10;

            buf[i * 2]     += left;
            buf[i * 2 + 1] += right;

            voice->phase += voice->carrier_freq;
            voice->mod_phase += voice->mod_freq;
        }

        // Decay envelope
        if (voice->env_decay > 0) {
            voice->env_level -= voice->env_decay;
            if (voice->env_level <= 0) {
                voice->env_level = 0;
                voice->active = 0;
            }
        }
    }
}

// ============================================================
// MIDI Sequencer
// ============================================================

typedef struct {
    unsigned char *data;     // raw MIDI file data
    size_t length;           // total length
    size_t track_start;      // offset to first track event
    size_t track_end;        // end of track data
    size_t pos;              // current read position
    unsigned int ticks_per_qn; // ticks per quarter note (from header)
    unsigned int tempo;      // microseconds per quarter note (default 500000 = 120 BPM)
    unsigned int tick_accum; // accumulated fractional ticks (Q16)
    unsigned int samples_per_tick_q16; // samples per MIDI tick in Q16
    int playing;
    int looping;
    unsigned int generation;   // incremented on each new song
} midi_state_t;

static midi_state_t midi_state;
static int music_volume = 127;  // 0-127

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
    // samples_per_tick = (tempo_us / 1000000) * MIX_FREQ / ticks_per_qn
    // In Q16: samples_per_tick_q16 = tempo * MIX_FREQ * 65536 / (1000000 * ticks_per_qn)
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
    m->tempo = 500000;  // 120 BPM default

    // Parse MIDI header
    if (len < 14 || memcmp(data, "MThd", 4) != 0) return false;

    unsigned int header_len = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    int format = (data[8] << 8) | data[9];
    int ntracks = (data[10] << 8) | data[11];
    m->ticks_per_qn = (data[12] << 8) | data[13];

    (void)format; // we only play track 0 (type 0 MIDI from mus2mid)

    // Find first track
    size_t offset = 8 + header_len;
    if (offset + 8 > len) return false;
    if (memcmp(data + offset, "MTrk", 4) != 0) return false;

    unsigned int track_len = (data[offset+4] << 24) | (data[offset+5] << 16)
                           | (data[offset+6] << 8)  | data[offset+7];

    m->track_start = offset + 8;
    m->track_end = m->track_start + track_len;
    if (m->track_end > len) m->track_end = len;

    m->pos = m->track_start;

    (void)ntracks;
    MidiRecalcTempo(m);
    return true;
}

// Process MIDI events up to the current time
static void MidiAdvance(midi_state_t *m, unsigned int samples)
{
    if (!m->playing) return;

    // Convert samples to ticks (Q16)
    m->tick_accum += ((uint64_t)samples << 16) / (m->samples_per_tick_q16 > 0 ? m->samples_per_tick_q16 : 1);

    // These are stored in the midi_state via a generation counter to
    // detect song changes and re-read the initial delay.
    static unsigned char running_status = 0;
    static unsigned int next_delay = 0;
    static int delay_initialized = 0;
    static unsigned int song_generation = 0;

    if (m->generation != song_generation) {
        // New song — reset sequencer state
        song_generation = m->generation;
        delay_initialized = 0;
        running_status = 0;
        next_delay = 0;
    }

    // Read initial delay if needed
    if (!delay_initialized) {
        next_delay = MidiReadVarLen(m);
        delay_initialized = 1;
    }

    while (m->tick_accum >= ((uint32_t)next_delay << 16)) {
        m->tick_accum -= ((uint32_t)next_delay << 16);

        if (m->pos >= m->track_end) {
            if (m->looping) {
                m->pos = m->track_start;
                delay_initialized = 0;
                running_status = 0;
                next_delay = MidiReadVarLen(m);
                delay_initialized = 1;
                // Reset all voices for clean loop
                for (int i = 0; i < FM_MAX_VOICES; i++)
                    fm_voices[i].active = 0;
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
            FmNoteOff(ch, note);
            break;
        }
        case 0x90: { // Note On
            int note = m->data[m->pos++] & 0x7F;
            int vel = m->data[m->pos++] & 0x7F;
            if (vel == 0)
                FmNoteOff(ch, note);
            else {
                // Scale velocity by music volume
                vel = (vel * music_volume) / 127;
                if (ch == 9) {
                    // Percussion channel — skip FM, could add noise later
                } else {
                    FmNoteOn(ch, note, vel);
                }
            }
            break;
        }
        case 0xA0: // Aftertouch
            m->pos += 2;
            break;
        case 0xB0: { // Control Change
            int cc = m->data[m->pos++] & 0x7F;
            int val = m->data[m->pos++] & 0x7F;
            switch (cc) {
                case 7:  midi_channels[ch].volume = val; break;
                case 10: midi_channels[ch].pan = val; break;
                case 11: midi_channels[ch].expression = val; break;
                case 120: // All Sound Off
                case 123: // All Notes Off
                    for (int i = 0; i < FM_MAX_VOICES; i++) {
                        if (fm_voices[i].midi_channel == ch)
                            fm_voices[i].active = 0;
                    }
                    break;
            }
            break;
        }
        case 0xC0: { // Program Change
            int prog = m->data[m->pos++] & 0x7F;
            midi_channels[ch].program = prog;
            break;
        }
        case 0xD0: // Channel Pressure
            m->pos++;
            break;
        case 0xE0: { // Pitch Bend
            int lsb = m->data[m->pos++];
            int msb = m->data[m->pos++];
            midi_channels[ch].pitch_bend = ((msb << 7) | lsb) - 8192;
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
                        for (int i = 0; i < FM_MAX_VOICES; i++)
                            fm_voices[i].active = 0;
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
            } else {
                // Unknown system message, skip
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
                next_delay = MidiReadVarLen(m);
                delay_initialized = 1;
                for (int i = 0; i < FM_MAX_VOICES; i++)
                    fm_voices[i].active = 0;
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
// Combined mixer (SFX + Music → /dev/dsp)
// ============================================================

static void MixAndWrite(void)
{
    int32_t mixbuf[MIX_BUFFER * 2];  // 32-bit accumulators
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

    // Render FM music into the same buffer
    if (midi_state.playing) {
        MidiAdvance(&midi_state, MIX_BUFFER);
        FmRender(mixbuf, MIX_BUFFER);
    }

    // Clamp to int16 and write
    int16_t outbuf[MIX_BUFFER * 2];
    for (int i = 0; i < MIX_BUFFER * 2; i++) {
        int val = mixbuf[i];
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        outbuf[i] = (int16_t)val;
    }

    if (dsp_fd >= 0) {
        static int mix_count = 0;
        mix_count++;

        // Check if buffer has any non-zero samples
        int maxAbs = 0;
        for (int i = 0; i < MIX_BUFFER * 2; i++) {
            int a = outbuf[i] < 0 ? -outbuf[i] : outbuf[i];
            if (a > maxAbs) maxAbs = a;
        }

        if ((mix_count % 100) == 1) {
            fprintf(stderr, "MixAndWrite #%d: %d bytes, peak=%d, midi=%d\n",
                    mix_count, (int)sizeof(outbuf), maxAbs,
                    midi_state.playing ? 1 : 0);
        }
        write(dsp_fd, outbuf, sizeof(outbuf));
    }
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

    channels[channel].vol_left  = (vol * left_frac) / (127 * 127);
    channels[channel].vol_right = (vol * right_frac) / (127 * 127);

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

    for (i = 0; i < FM_MAX_VOICES; i++)
        fm_voices[i].active = 0;

    for (i = 0; i < 16; i++) {
        midi_channels[i].program = 0;
        midi_channels[i].volume = 100;
        midi_channels[i].pan = 64;
        midi_channels[i].expression = 127;
        midi_channels[i].pitch_bend = 0;
    }

    FmInitSineTable();
    FmInitFreqTable();

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

    fprintf(stderr, "I_OSS_InitSound: /dev/dsp opened, %d Hz stereo 16-bit (FM music enabled)\n",
            MIX_FREQ);

    sound_initialized = true;
    return true;
}

// ============================================================
// Music module interface
// ============================================================

static boolean music_initialized = false;
// Store converted MIDI data per registered song
typedef struct {
    unsigned char *midi_data;
    size_t midi_len;
} registered_song_t;

static boolean I_OSS_InitMusic(void)
{
    music_initialized = true;
    fprintf(stderr, "I_OSS_InitMusic: FM synthesis music enabled\n");
    return true;
}

static void I_OSS_ShutdownMusic(void)
{
    midi_state.playing = 0;
    music_initialized = false;
}

static void I_OSS_SetMusicVolume(int volume)
{
    music_volume = volume;
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
            fprintf(stderr, "I_OSS_RegisterSong: MUS to MIDI conversion failed\n");
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
        fprintf(stderr, "I_OSS_RegisterSong: unknown music format\n");
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

    // If this song is currently playing, stop it
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
    for (int i = 0; i < FM_MAX_VOICES; i++)
        fm_voices[i].active = 0;

    // Reset MIDI channel state
    for (int i = 0; i < 16; i++) {
        midi_channels[i].program = 0;
        midi_channels[i].volume = 100;
        midi_channels[i].pan = 64;
        midi_channels[i].expression = 127;
        midi_channels[i].pitch_bend = 0;
    }

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
    for (int i = 0; i < FM_MAX_VOICES; i++)
        fm_voices[i].active = 0;
}

static boolean I_OSS_MusicIsPlaying(void)
{
    return midi_state.playing;
}

static void I_OSS_PollMusic(void)
{
    // Music is rendered inline in MixAndWrite(), nothing extra needed here
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
