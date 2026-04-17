// tone.c — Generate a sine wave tone and play via /dev/dsp (OSS).
//
// Usage: tone [freq_hz] [duration_ms]
// Default: 440 Hz for 1000ms

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define SNDCTL_DSP_SPEED     _IOWR('P', 2, int)
#define SNDCTL_DSP_SETFMT    _IOWR('P', 5, int)
#define SNDCTL_DSP_CHANNELS  _IOWR('P', 6, int)
#define AFMT_S16_LE          0x00000010

// 256-entry sine table (Q15)
static const int16_t sinTable[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

int main(int argc, char* argv[])
{
    uint32_t freq = 440;
    uint32_t durationMs = 1000;
    if (argc >= 2) freq = (uint32_t)atoi(argv[1]);
    if (argc >= 3) durationMs = (uint32_t)atoi(argv[2]);
    if (freq < 20) freq = 20;
    if (freq > 20000) freq = 20000;

    int dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0) { fprintf(stderr, "Cannot open /dev/dsp\n"); return 1; }

    int rate = 48000;
    ioctl(dsp, SNDCTL_DSP_SPEED, &rate);
    int fmt = AFMT_S16_LE;
    ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt);
    int ch = 1;
    ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch);

    uint32_t numSamples = ((uint32_t)rate * durationMs) / 1000;
    uint32_t phaseInc = (freq * 256 * 256) / (uint32_t)rate;
    uint32_t phase = 0;

    // Write in chunks
    int16_t buf[4096];
    uint32_t written = 0;
    while (written < numSamples) {
        uint32_t chunk = numSamples - written;
        if (chunk > 4096) chunk = 4096;
        for (uint32_t i = 0; i < chunk; i++) {
            buf[i] = sinTable[(phase >> 8) & 0xFF] / 2;
            phase += phaseInc;
        }
        write(dsp, buf, chunk * 2);
        written += chunk;
    }

    close(dsp);
    printf("%u Hz for %u ms\n", freq, durationMs);
    return 0;
}
