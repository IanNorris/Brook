/* Exercise the installed ALSA route without changing system volume or mute.
 * Callers must separately ensure that their selected output is safe to use. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct _snd_pcm snd_pcm_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
extern int snd_pcm_open(snd_pcm_t **, const char *, int, int);
extern int snd_pcm_set_params(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
extern snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *, snd_pcm_uframes_t);
extern int snd_pcm_delay(snd_pcm_t *, snd_pcm_sframes_t *);
extern int snd_pcm_drain(snd_pcm_t *);
extern int snd_pcm_close(snd_pcm_t *);
extern const char *snd_strerror(int);

static int fail(const char *operation, long error) {
    fprintf(stderr, "ALSA_PROBE_FAIL %s error=%ld %s\n", operation, error,
            snd_strerror((int)error));
    return 1;
}

int main(int argc, char **argv) {
    unsigned seconds = 5, rate = 44100;
    if (argc > 3) return 2;
    if (argc >= 2) {
        char *end;
        unsigned long value = strtoul(argv[1], &end, 10);
        if (!*argv[1] || *end || value < 1 || value > 30) return 2;
        seconds = (unsigned)value;
    }
    if (argc == 3) {
        char *end;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || (value != 44100 && value != 48000)) return 2;
        rate = (unsigned)value;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    snd_pcm_t *pcm = NULL;
    int result = snd_pcm_open(&pcm, "default", 0, 0); /* playback */
    if (result < 0) return fail("open", result);
    /* S16_LE, RW_INTERLEAVED, stereo, 200 ms latency. */
    result = snd_pcm_set_params(pcm, 2, 3, 2, rate, 1, 200000);
    if (result < 0) { snd_pcm_close(pcm); return fail("configure", result); }
    printf("ALSA_PROBE_CONFIG rate=%u channels=2 seconds=%u\n", rate, seconds);
    int16_t samples[1024 * 2];
    unsigned long total = 0, goal = (unsigned long)rate * seconds;
    while (total < goal) {
        unsigned frames = goal - total > 1024 ? 1024 : (unsigned)(goal - total);
        for (unsigned i = 0; i < frames; ++i) {
            int16_t value = (((total + i) / 50) & 1) ? 2000 : -2000;
            samples[2 * i] = samples[2 * i + 1] = value;
        }
        unsigned offset = 0;
        while (offset < frames) {
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, samples + 2 * offset, frames - offset);
            if (n <= 0 || (unsigned long)n > frames - offset) {
                snd_pcm_close(pcm);
                return fail("write", n < 0 ? n : -5);
            }
            offset += (unsigned)n;
        }
        total += frames;
    }
    snd_pcm_sframes_t delay = 0;
    result = snd_pcm_delay(pcm, &delay);
    if (result < 0 || delay < 0) { snd_pcm_close(pcm); return fail("delay", result < 0 ? result : -5); }
    printf("ALSA_PROBE_WRITTEN frames=%lu delay=%ld\n", total, delay);
    result = snd_pcm_drain(pcm);
    if (result < 0) { snd_pcm_close(pcm); return fail("drain", result); }
    result = snd_pcm_close(pcm);
    if (result < 0) return fail("close", result);
    puts("ALSA_PROBE_OK");
    return 0;
}
