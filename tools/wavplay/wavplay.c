// wavplay.c — Simple WAV file player for Brook OS via /dev/dsp (OSS).
//
// Usage: wavplay <file.wav>
//
// Parses the WAV RIFF header, configures /dev/dsp with the correct
// sample rate, channels, and bits per sample, then streams PCM data.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

// OSS ioctl definitions (subset needed for playback)
#define SNDCTL_DSP_SPEED     _IOWR('P', 2, int)
#define SNDCTL_DSP_SETFMT    _IOWR('P', 5, int)
#define SNDCTL_DSP_CHANNELS  _IOWR('P', 6, int)
#define AFMT_U8              0x00000008
#define AFMT_S16_LE          0x00000010

struct WavHeader {
    char     riffId[4];      // "RIFF"
    uint32_t fileSize;
    char     waveId[4];      // "WAVE"
};

struct WavChunk {
    char     id[4];
    uint32_t size;
};

struct WavFmt {
    uint16_t audioFormat;    // 1 = PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: wavplay <file.wav>\n");
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", argv[1]);
        return 1;
    }

    // Read RIFF header
    struct WavHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        memcmp(hdr.riffId, "RIFF", 4) != 0 ||
        memcmp(hdr.waveId, "WAVE", 4) != 0) {
        fprintf(stderr, "Not a valid WAV file\n");
        fclose(f);
        return 1;
    }

    // Find fmt and data chunks
    struct WavFmt fmt = {0};
    uint32_t dataSize = 0;
    int foundFmt = 0, foundData = 0;

    while (!foundData) {
        struct WavChunk chunk;
        if (fread(&chunk, sizeof(chunk), 1, f) != 1)
            break;

        if (memcmp(chunk.id, "fmt ", 4) == 0) {
            uint32_t readSize = chunk.size < sizeof(fmt) ? chunk.size : sizeof(fmt);
            if (fread(&fmt, readSize, 1, f) != 1) break;
            // Skip extra fmt bytes
            if (chunk.size > readSize)
                fseek(f, chunk.size - readSize, SEEK_CUR);
            foundFmt = 1;
        } else if (memcmp(chunk.id, "data", 4) == 0) {
            dataSize = chunk.size;
            foundData = 1;
        } else {
            // Skip unknown chunk
            fseek(f, chunk.size, SEEK_CUR);
        }
    }

    if (!foundFmt || !foundData) {
        fprintf(stderr, "WAV file missing fmt or data chunk\n");
        fclose(f);
        return 1;
    }

    if (fmt.audioFormat != 1) {
        fprintf(stderr, "Only PCM WAV files supported (got format %d)\n", fmt.audioFormat);
        fclose(f);
        return 1;
    }

    printf("WAV: %u Hz, %u-bit, %s, %u bytes PCM\n",
           fmt.sampleRate, fmt.bitsPerSample,
           fmt.numChannels == 2 ? "stereo" : "mono",
           dataSize);

    // Open /dev/dsp
    int dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0) {
        fprintf(stderr, "Cannot open /dev/dsp\n");
        fclose(f);
        return 1;
    }

    // Configure audio format
    int rate = (int)fmt.sampleRate;
    ioctl(dsp, SNDCTL_DSP_SPEED, &rate);

    int afmt = (fmt.bitsPerSample == 16) ? AFMT_S16_LE : AFMT_U8;
    ioctl(dsp, SNDCTL_DSP_SETFMT, &afmt);

    int channels = (int)fmt.numChannels;
    ioctl(dsp, SNDCTL_DSP_CHANNELS, &channels);

    // Stream audio data
    uint8_t buf[4096];
    uint32_t remaining = dataSize;
    while (remaining > 0) {
        uint32_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t got = fread(buf, 1, toRead, f);
        if (got == 0) break;
        write(dsp, buf, got);
        remaining -= (uint32_t)got;
    }

    close(dsp);
    fclose(f);
    printf("Playback complete\n");
    return 0;
}
