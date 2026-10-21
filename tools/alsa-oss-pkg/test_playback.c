/* Real libasound write/avail/delay/start/drain and rate conversion with
 * deterministic delayed OSS completion. No host audio or wall-clock sleeps. */
#undef NDEBUG
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    unsigned minimum = argc == 3 ? strtoul(argv[2], NULL, 10) : 64;
    assert(minimum >= 64 && minimum <= 16384);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    void (*geometry)(unsigned) = dlsym(library, "playback_geometry");
    void (*advance)(unsigned) = dlsym(library, "playback_advance");
    unsigned long long (*stat)(unsigned) = dlsym(library, "playback_stat");
    assert(geometry && advance && stat);
    char text[4096];
    int length = snprintf(text, sizeof(text),
        "pcm_type.oss { lib \"%s\" }\n"
        "pcm.test { type plug slave { pcm { type oss device /dev/null } "
        "format S16_LE rate 44100 channels 2 } }\n", argv[1]);
    assert(length > 0 && length < (int)sizeof(text));
    snd_config_t *config;
    snd_input_t *input;
    assert(snd_config_top(&config) == 0);
    assert(snd_input_buffer_open(&input, text, length) == 0);
    assert(snd_config_load(config, input) == 0);
    assert(snd_input_close(input) == 0);
    unsigned cases = 0;
    const unsigned rates[] = {22050, 44100, 48000, 96000};
    unsigned geometries=2;
#ifdef BROOK_OSS_DOWNSTREAM
    geometries=4;
#endif
    for (unsigned enlarged = 0; enlarged < geometries; ++enlarged)
    for (unsigned r = 0; r < sizeof(rates)/sizeof(rates[0]); ++r)
    for (unsigned f = 0; f < 2; ++f) {
        geometry(enlarged);
        snd_pcm_t *pcm;
        assert(snd_pcm_open_lconf(&pcm, "test", SND_PCM_STREAM_PLAYBACK,
            SND_PCM_NONBLOCK, config) == 0);
        snd_pcm_format_t format = f ? SND_PCM_FORMAT_FLOAT_LE : SND_PCM_FORMAT_S16_LE;
        assert(snd_pcm_set_params(pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
            2, rates[r], 1, 100000) == 0);
        snd_pcm_uframes_t buffer, period;
        assert(snd_pcm_get_params(pcm, &buffer, &period) == 0);
        snd_pcm_sw_params_t *sw;
        snd_pcm_sw_params_alloca(&sw);
        assert(snd_pcm_sw_params_current(pcm, sw) == 0);
        assert(snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer) == 0);
        assert(snd_pcm_sw_params(pcm, sw) == 0);
        size_t sample_bytes = snd_pcm_format_physical_width(format) / 8;
        void *samples = calloc(buffer * 2, 2 * sample_bytes);
        assert(samples);
        unsigned long long submitted = 0;
        const unsigned long long target = (unsigned long long)rates[r] * 4;
        unsigned steps = 0, partial = 0, blocked = 0, running = 0;
        unsigned extra_delay = 0, downstream_only = 0, reused_before_completion = 0;
        while (submitted < target) {
            assert(++steps < 10000);
            /* Alternate whole-ring, period-sized and oversized requests.
             * Delayed completion is explicit, so no blind immediate drain. */
            if(enlarged>=2 && steps<=8) {
                /* Allow actual libasound to refill reusable storage while
                 * physical completion remains frozen at zero. */
            } else if(enlarged>=2 && steps%40==0) {
                /* Leave only downstream source: client occupancy is zero
                 * while physical delay remains nonzero. */
                if(stat(2)>stat(9))advance(stat(2)-stat(9));
            } else if (steps % 4) advance((steps % 3 == 0) ? 128 : 441);
            snd_pcm_sframes_t available = snd_pcm_avail_update(pcm), delay;
            if (available < 0) {
                fprintf(stderr, "avail error=%ld step=%u queued=%llu logical=%llu\n",
                    available, steps, stat(2), stat(8));
                return 1;
            }
            assert(snd_pcm_delay(pcm, &delay) == 0 && delay >= 0);
            extra_delay |= stat(2)>stat(8);
            downstream_only |= stat(2)>0 && stat(10)==0;
            reused_before_completion |= stat(3)>stat(8) && stat(4)==0;
            snd_pcm_uframes_t request = steps % 3 == 0 ? period : buffer * (steps % 3 == 1 ? 2 : 1);
            if (request > target - submitted) request = target - submitted;
            snd_pcm_sframes_t result = snd_pcm_writei(pcm, samples, request);
            if (result == -EAGAIN) { ++blocked; continue; }
            if (result < 0) {
                fprintf(stderr, "write error=%ld step=%u queued=%llu logical=%llu\n",
                    result, steps, stat(2), stat(8));
                return 1;
            }
            assert((snd_pcm_uframes_t)result <= request);
            partial += (snd_pcm_uframes_t)result < request;
            submitted += result;
            running |= snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING;
            assert(stat(7) == 0);
        }
        assert(running && stat(4) > 44100);
        assert(snd_pcm_nonblock(pcm, 0) == 0);
        assert(snd_pcm_drain(pcm) == 0);
        if (minimum == 64) {
            /* Preserve the original regression controls exactly. */
            assert(stat(0) == ((enlarged%2) ? 6016 : 4416));
            assert(stat(1) == ((enlarged%2) ? 128 : 64));
            assert(stat(8) >= 4410 && stat(8) <= 4412);
        } else {
            unsigned expected = minimum > 4410 ? minimum : 4410;
            assert(stat(8) >= expected && stat(8) <= expected + 2);
            unsigned long long physical = ((stat(8) + 63) / 64) * 64;
            int enlarged_control = (enlarged%2) && physical == 4416;
            assert(stat(1) == (enlarged_control ? 128 : 64));
            assert(stat(0) == (enlarged_control ? 6016 : physical));
        }
        assert(stat(2) == 0 && stat(3) == stat(4));
#ifdef BROOK_OSS_DOWNSTREAM
        if(enlarged>=2) assert(extra_delay && downstream_only && reused_before_completion);
#endif
        printf("OSS_PLAYBACK geometry=%u rate=%u format=%s buffer=%lu period=%lu "
            "physical=%llu fragment=%llu logical=%llu submitted=%llu "
            "accepted=%llu completed=%llu partial=%u blocked=%u overfills=%llu\n",
            enlarged, rates[r], snd_pcm_format_name(format), buffer, period,
            stat(0), stat(1), stat(8), submitted, stat(3), stat(4), partial, blocked, stat(7));
        assert(stat(5) > 10 && partial > 0 && submitted == target);
        assert(snd_pcm_close(pcm) == 0);
        free(samples);
        ++cases;
    }
    assert(snd_config_delete(config) == 0);
    assert(dlclose(library) == 0);
    printf("OSS_SUSTAINED_PLAYBACK cases=%u PASS\n", cases);
    return 0;
}
