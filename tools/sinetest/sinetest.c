// sinetest — Sine wave frequency sweep for Brook OS audio testing
//
// Generates a 10-second sweep from 200Hz to 4000Hz, written to /dev/dsp
// at 44100Hz stereo 16-bit. If audio pipeline works, you hear a rising tone.
//
// Usage: sinetest

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define AFMT_S16_LE           0x00000010

#define SAMPLE_RATE  44100
#define CHANNELS     2
#define DURATION_SEC 10
#define FREQ_START   200.0
#define FREQ_END     4000.0
#define AMPLITUDE    24000

// Simple fixed-point sine approximation (no libm needed)
// Uses a polynomial approximation of sin(x) for x in [-pi, pi]
static int sine_fixed(unsigned int phase)
{
    // phase is 0..65535 representing 0..2*pi
    // Convert to signed -32768..32767
    int x = (int)(short)phase;

    // Normalize to -1..1 range (as Q15)
    // sin approximation: x*(1 - x^2/6 + x^4/120) scaled
    // But simpler: use parabolic approximation
    // sin(x) ~= 4/pi * x - 4/pi^2 * x * |x| for x in [-pi, pi]

    // Map phase to -32768..32767 sawtooth
    int y;
    if (x >= 0) {
        // First half: 0..32767 -> parabola
        y = (int)(32767 - ((long long)x * x * 2LL) / 32768);
    } else {
        // Second half: -32768..0 -> parabola
        y = (int)(-32767 + ((long long)x * x * 2LL) / 32768);
    }

    // Apply smoothing for better sine shape
    // Weighted average with cubic correction
    int abs_y = y < 0 ? -y : y;
    y = (int)(((long long)y * (3 * 32768LL - abs_y)) / (2 * 32768LL));

    return y;
}

int main(void)
{
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "sinetest: cannot open /dev/dsp\n");
        return 1;
    }

    int val;
    val = AFMT_S16_LE;
    ioctl(fd, SNDCTL_DSP_SETFMT, &val);
    val = CHANNELS;
    ioctl(fd, SNDCTL_DSP_CHANNELS, &val);
    val = SAMPLE_RATE;
    ioctl(fd, SNDCTL_DSP_SPEED, &val);

    fprintf(stderr, "sinetest: %dHz stereo 16-bit, sweep %d-%dHz over %ds\n",
            SAMPLE_RATE, (int)FREQ_START, (int)FREQ_END, DURATION_SEC);

    unsigned int totalSamples = SAMPLE_RATE * DURATION_SEC;
    unsigned int phase = 0;

    // Write in chunks of 512 frames (2048 bytes stereo 16-bit)
    #define CHUNK_FRAMES 512
    short buf[CHUNK_FRAMES * CHANNELS];
    unsigned int samplePos = 0;

    while (samplePos < totalSamples) {
        unsigned int frames = CHUNK_FRAMES;
        if (samplePos + frames > totalSamples)
            frames = totalSamples - samplePos;

        for (unsigned int i = 0; i < frames; i++) {
            // Linearly interpolate frequency
            double t = (double)(samplePos + i) / (double)totalSamples;
            double freq = FREQ_START + (FREQ_END - FREQ_START) * t;

            // Phase increment per sample (16-bit fixed point, 65536 = 2*pi)
            unsigned int phaseInc = (unsigned int)(freq * 65536.0 / SAMPLE_RATE);
            phase += phaseInc;

            int sample = (sine_fixed(phase & 0xFFFF) * AMPLITUDE) / 32768;
            buf[i * 2]     = (short)sample;  // left
            buf[i * 2 + 1] = (short)sample;  // right
        }

        write(fd, buf, frames * CHANNELS * sizeof(short));
        samplePos += frames;
    }

    fprintf(stderr, "sinetest: done\n");
    close(fd);
    return 0;
}
