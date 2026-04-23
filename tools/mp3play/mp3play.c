// mp3play — Simple MP3 player for Brook OS
//
// Uses minimp3 (public domain) for decoding and OSS /dev/dsp for output.
// Reads entire MP3 file into memory, decodes frame by frame, writes PCM
// to /dev/dsp.
//
// Usage: mp3play <file.mp3>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

static volatile int g_quit = 0;

static void sighandler(int sig)
{
    (void)sig;
    g_quit = 1;
}

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// OSS ioctl definitions
#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define AFMT_S16_LE           0x00000010

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <file.mp3>\n", prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    // Read entire MP3 file into memory
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "mp3play: cannot open %s\n", path);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        fprintf(stderr, "mp3play: cannot stat %s\n", path);
        close(fd);
        return 1;
    }

    size_t file_size = (size_t)st.st_size;
    unsigned char *mp3_data = malloc(file_size);
    if (!mp3_data) {
        fprintf(stderr, "mp3play: out of memory (%zu bytes)\n", file_size);
        close(fd);
        return 1;
    }

    size_t total_read = 0;
    while (total_read < file_size) {
        ssize_t n = read(fd, mp3_data + total_read, file_size - total_read);
        if (n <= 0) break;
        total_read += (size_t)n;
    }
    close(fd);

    if (total_read == 0) {
        fprintf(stderr, "mp3play: empty file\n");
        free(mp3_data);
        return 1;
    }

    // Initialize minimp3 decoder
    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    // Decode first frame to get sample rate and channels
    mp3dec_frame_info_t info;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    int samples = mp3dec_decode_frame(&mp3d, mp3_data, (int)total_read,
                                       pcm, &info);

    if (info.frame_bytes == 0) {
        fprintf(stderr, "mp3play: not a valid MP3 file\n");
        free(mp3_data);
        return 1;
    }

    int sample_rate = info.hz;
    int channels = info.channels;

    fprintf(stderr, "mp3play: %s — %d Hz, %d channel%s, %d kbps\n",
            path, sample_rate, channels, channels > 1 ? "s" : "",
            info.bitrate_kbps);

    // Open /dev/dsp
    int dsp_fd = open("/dev/dsp", O_WRONLY);
    if (dsp_fd < 0) {
        fprintf(stderr, "mp3play: cannot open /dev/dsp\n");
        free(mp3_data);
        return 1;
    }

    // Configure OSS
    int val = AFMT_S16_LE;
    ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &val);

    val = channels;
    ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &val);

    val = sample_rate;
    ioctl(dsp_fd, SNDCTL_DSP_SPEED, &val);

    // Write the first decoded frame
    if (samples > 0) {
        ssize_t w = write(dsp_fd, pcm, (size_t)(samples * channels * sizeof(short)));
        if (w < 0) { close(dsp_fd); free(mp3_data); return 1; }
    }

    // Decode and play remaining frames
    size_t offset = (size_t)info.frame_bytes;
    unsigned long total_samples = (unsigned long)samples;

    while (offset < total_read && !g_quit) {
        samples = mp3dec_decode_frame(&mp3d, mp3_data + offset,
                                       (int)(total_read - offset),
                                       pcm, &info);

        if (info.frame_bytes == 0)
            break;  // no more frames

        offset += (size_t)info.frame_bytes;

        if (samples > 0) {
            size_t bytes = (size_t)(samples * channels * sizeof(short));
            ssize_t w = write(dsp_fd, pcm, bytes);
            if (w < 0) break;
            total_samples += (unsigned long)samples;
        }
    }

    close(dsp_fd);
    free(mp3_data);

    float duration = (float)total_samples / (float)sample_rate;
    fprintf(stderr, "mp3play: done — %.1f seconds (%lu samples)\n",
            duration, total_samples);

    return 0;
}
