#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <alsa/asoundlib.h>
int main(int argc, char **argv) {
    assert(argc == 2);
    char text[4096];
    int length = snprintf(text, sizeof(text),
        "pcm_type.oss { lib \"%s\" }\n"
        "pcm.test { type plug slave { pcm { type oss device /dev/null } "
        "format S16_LE rate 44100 channels 2 } }\n", argv[1]);
    assert(length > 0 && (size_t)length < sizeof(text));
    snd_config_t *config;
    snd_input_t *input;
    assert(snd_config_top(&config) == 0);
    assert(snd_input_buffer_open(&input, text, length) == 0);
    assert(snd_config_load(config, input) == 0);
    assert(snd_input_close(input) == 0);
    const unsigned rates[] = {8000, 22050, 44100, 48000, 96000};
    const unsigned latencies[] = {50000, 100000, 200000};
    const snd_pcm_format_t formats[] = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_FLOAT_LE};
    unsigned failed = 0, cases = 0;
    for (unsigned r = 0; r < 5; ++r) for (unsigned f = 0; f < 2; ++f)
        for (unsigned l = 0; l < 3; ++l) {
            snd_pcm_t *pcm = NULL;
            int result = snd_pcm_open_lconf(&pcm, "test", SND_PCM_STREAM_PLAYBACK, 0, config);
            if (result >= 0) result = snd_pcm_set_params(pcm, formats[f],
                SND_PCM_ACCESS_RW_INTERLEAVED, 2, rates[r], 1, latencies[l]);
            if (result < 0) ++failed;
            printf("rate=%u format=%s latency_us=%u result=%d (%s)\n", rates[r],
                snd_pcm_format_name(formats[f]), latencies[l], result, snd_strerror(result));
            if (pcm) assert(snd_pcm_close(pcm) == 0);
            ++cases;
        }
    assert(snd_config_delete(config) == 0);
    printf("OSS_RATE_NEGOTIATION cases=%u failed=%u\n", cases, failed);
    return failed ? 1 : 0;
}
