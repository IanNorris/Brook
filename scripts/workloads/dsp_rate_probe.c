/* Compare the playback position Brook reports to an application with the
 * guest monotonic clock while a stream plays.
 *
 * OSS mode (static musl build, no runtime dependencies):
 *   dsp-rate-probe oss SECONDS RATE AMPLITUDE
 * ALSA mode (dynamic build against the pinned alsa-lib; uses ALSA_CONFIG_PATH,
 * so it takes the same plug -> clocked OSS route as the browser wrappers):
 *   dsp-rate-probe-alsa alsa SECONDS RATE AMPLITUDE [PCM]
 *
 * AMPLITUDE is a peak in [0, 2000]; 0 writes silence, which still runs the
 * device clock. Speaker output additionally needs the user unmute control, so
 * this probe never changes volume or mute. Every 100 ms it prints one JSON line
 * with the guest CLOCK_MONOTONIC time, frames written, the reported delay and,
 * where available, the OSS GETOPTR byte pointer. The summary fits the played
 * position (written - delay) against guest time after a one-second warm-up:
 * position_rate should equal RATE. Compare guest_elapsed_ns with the host
 * brackets of the job to check the guest clock itself.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#ifdef WITH_ALSA
#include <alsa/asoundlib.h>
#endif

#define SNDCTL_DSP_SPEED 0xC0045002u
#define SNDCTL_DSP_SETFMT 0xC0045005u
#define SNDCTL_DSP_CHANNELS 0xC0045006u
#define SNDCTL_DSP_GETODELAY 0x80045017u
#define SNDCTL_DSP_GETOPTR 0x800C5012u
#define SNDCTL_DSP_SYNC 0x5001u
#define AFMT_S16_LE 0x10

static uint64_t now_ns(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

struct fit { double n, sx, sy, sxx, sxy; };
static void fit_add(struct fit *f, double x, double y) {
    f->n += 1; f->sx += x; f->sy += y; f->sxx += x * x; f->sxy += x * y;
}
static double fit_slope(const struct fit *f) {
    const double d = f->n * f->sxx - f->sx * f->sx;
    return f->n >= 2 && d != 0 ? (f->n * f->sxy - f->sx * f->sy) / d : 0;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s oss|alsa SECONDS RATE AMPLITUDE [PCM]\n", argv[0]);
        return 2;
    }
    const int alsa = !strcmp(argv[1], "alsa");
    const unsigned seconds = (unsigned)strtoul(argv[2], 0, 10);
    unsigned rate = (unsigned)strtoul(argv[3], 0, 10);
    const int amplitude = atoi(argv[4]);
    if ((!alsa && strcmp(argv[1], "oss")) || seconds < 2 || seconds > 60 || rate < 8000 || rate > 96000 ||
        amplitude < 0 || amplitude > 2000)
        return 2;
#ifndef WITH_ALSA
    if (alsa) { fprintf(stderr, "PROBE_FAIL built without ALSA\n"); return 2; }
#endif
    int fd = -1;
#ifdef WITH_ALSA
    snd_pcm_t *pcm = 0;
#endif
    if (alsa) {
#ifdef WITH_ALSA
        int e = snd_pcm_open(&pcm, argc > 5 ? argv[5] : "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (e >= 0) e = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 100000);
        if (e < 0) { fprintf(stderr, "PROBE_FAIL alsa open/params %d %s\n", e, snd_strerror(e)); return 1; }
#endif
    } else {
        fd = open("/dev/dsp", O_WRONLY);
        if (fd < 0) { fprintf(stderr, "PROBE_FAIL open /dev/dsp errno=%d %s\n", errno, strerror(errno)); return 1; }
        int v = AFMT_S16_LE;
        if (ioctl(fd, SNDCTL_DSP_SETFMT, &v) < 0 || v != AFMT_S16_LE) { fprintf(stderr, "PROBE_FAIL setfmt\n"); return 1; }
        v = 2;
        if (ioctl(fd, SNDCTL_DSP_CHANNELS, &v) < 0 || v != 2) { fprintf(stderr, "PROBE_FAIL channels\n"); return 1; }
        v = (int)rate;
        if (ioctl(fd, SNDCTL_DSP_SPEED, &v) < 0) { fprintf(stderr, "PROBE_FAIL speed errno=%d\n", errno); return 1; }
        if ((unsigned)v != rate) fprintf(stderr, "PROBE_NOTE speed negotiated %d\n", v);
        rate = (unsigned)v;
    }
    enum { Chunk = 441 };  /* 10 ms at 44.1 kHz; any rate is fine */
    static int16_t buf[Chunk * 2];
    const uint64_t total = (uint64_t)seconds * rate;
    uint64_t written = 0, phase = 0, next_sample = 0, zero_delay = 0, samples = 0;
    long long first_optr = -1;
    struct fit pos = {0}, optr = {0};
    const uint64_t m0 = now_ns(CLOCK_MONOTONIC), r0 = now_ns(CLOCK_REALTIME);
    printf("{\"start\":{\"mode\":\"%s\",\"rate\":%u,\"seconds\":%u,\"monotonic_ns\":%llu,\"realtime_ns\":%llu}}\n",
           argv[1], rate, seconds, (unsigned long long)m0, (unsigned long long)r0);
    fflush(stdout);
    while (written < total) {
        for (unsigned i = 0; i < Chunk; ++i, ++phase) {
            const int16_t s = amplitude ? (int16_t)(amplitude * sin(2 * M_PI * 440.0 * (double)phase / rate)) : 0;
            buf[2 * i] = buf[2 * i + 1] = s;
        }
        long done;
#ifdef WITH_ALSA
        if (alsa) {
            done = snd_pcm_writei(pcm, buf, Chunk);
            if (done == -EPIPE) { fprintf(stderr, "PROBE_NOTE alsa underrun at %llu\n", (unsigned long long)written);
                snd_pcm_prepare(pcm); continue; }
            if (done < 0) { fprintf(stderr, "PROBE_FAIL writei %ld %s\n", done, snd_strerror((int)done)); return 1; }
        } else
#endif
        {
            const ssize_t n = write(fd, buf, sizeof(buf));
            if (n < 0) { if (errno == EINTR) continue; fprintf(stderr, "PROBE_FAIL write errno=%d\n", errno); return 1; }
            done = n / 4;
        }
        written += (uint64_t)done;
        const uint64_t t = now_ns(CLOCK_MONOTONIC) - m0;
        if (t < next_sample) continue;
        next_sample = t + 100000000ull;
        long delay = -1;
        long long ptr = -1;
#ifdef WITH_ALSA
        if (alsa) { snd_pcm_sframes_t d = 0; if (snd_pcm_delay(pcm, &d) == 0) delay = d; }
        else
#endif
        {
            int d = 0;
            if (ioctl(fd, SNDCTL_DSP_GETODELAY, &d) == 0) delay = d / 4;
            struct { int bytes, blocks, ptr; } info;
            if (ioctl(fd, SNDCTL_DSP_GETOPTR, &info) == 0) ptr = info.bytes;
        }
        if (delay == 0) ++zero_delay;
        printf("{\"t_ns\":%llu,\"written\":%llu,\"delay\":%ld,\"optr_bytes\":%lld}\n",
               (unsigned long long)t, (unsigned long long)written, delay, ptr);
        if (t >= 1000000000ull && delay >= 0) {
            fit_add(&pos, t / 1e9, (double)written - (double)delay);
            if (ptr >= 0) { if (first_optr < 0) first_optr = ptr; fit_add(&optr, t / 1e9, (double)(ptr - first_optr) / 4); }
            ++samples;
        }
    }
    const uint64_t m1 = now_ns(CLOCK_MONOTONIC), r1 = now_ns(CLOCK_REALTIME);
#ifdef WITH_ALSA
    if (alsa) { snd_pcm_drain(pcm); snd_pcm_close(pcm); }
    else
#endif
    { ioctl(fd, SNDCTL_DSP_SYNC, 0); close(fd); }
    const uint64_t m2 = now_ns(CLOCK_MONOTONIC);
    const double pr = fit_slope(&pos), orr = fit_slope(&optr);
    printf("{\"summary\":{\"mode\":\"%s\",\"rate\":%u,\"frames_written\":%llu,\"samples\":%llu,"
           "\"position_rate\":%.1f,\"position_ratio\":%.5f,\"optr_rate\":%.1f,\"zero_delay_samples\":%llu,"
           "\"guest_elapsed_ns\":%llu,\"realtime_elapsed_ns\":%llu,\"drain_ns\":%llu,"
           "\"expected_write_s\":%.3f}}\n",
           argv[1], rate, (unsigned long long)written, (unsigned long long)samples, pr, pr / rate, orr,
           (unsigned long long)zero_delay, (unsigned long long)(m1 - m0), (unsigned long long)(r1 - r0),
           (unsigned long long)(m2 - m1), (double)written / rate);
    return 0;
}
