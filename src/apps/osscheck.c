// Live ABI probe: request numbers come from the toolchain's Linux headers.
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) { perror("osscheck: open"); return 1; }
    int rate = 44100, format = AFMT_S16_LE, channels = 2;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0 ||
        ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 ||
        ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0) {
        perror("osscheck: setup"); close(fd); return 1;
    }
    audio_buf_info space = {0};
    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &space) < 0) {
        perror("osscheck: GETOSPACE"); close(fd); return 1;
    }
    if (space.fragments < 0 || space.fragstotal <= 0 ||
        space.fragsize <= 0 || space.fragments > space.fragstotal ||
        space.bytes < 0 || space.bytes > space.fragstotal * space.fragsize) {
        fprintf(stderr, "osscheck: invalid buffer space %d/%d x %d, bytes=%d\n",
                space.fragments, space.fragstotal, space.fragsize, space.bytes);
        close(fd); return 1;
    }
    printf("osscheck: PASS GETOSPACE=0x%lx bytes=%d fragments=%d/%d\n",
           (unsigned long)SNDCTL_DSP_GETOSPACE, space.bytes,
           space.fragments, space.fragstotal);
    if (argc > 1 && !strcmp(argv[1], "--stream")) {
        // A one-second write is larger than the mixer ring. The syscall must
        // return a short count, not acknowledge samples that it discarded.
        static int16_t pcm[44100 * 2];
        for (unsigned i=0; i<44100; ++i) {
            int16_t value=(int16_t)((i % 1024) * 16 - 8192);
            pcm[i*2]=value; pcm[i*2+1]=value;
        }
        size_t sent=0; unsigned calls=0, shorts=0; ssize_t first=0;
        while (sent<sizeof(pcm)) {
            size_t remaining=sizeof(pcm)-sent;
            ssize_t written=write(fd, (char *)pcm+sent, remaining);
            if (written<0 && (errno==EAGAIN || errno==EINTR)) { usleep(5000); continue; }
            if (written<=0 || (size_t)written>remaining) {
                perror("osscheck: stream write"); close(fd); return 1;
            }
            if (!calls) first=written;
            if ((size_t)written<remaining) ++shorts;
            ++calls; sent+=(size_t)written;
        }
        usleep(600000); // let the final queued fragment reach HDA before close
        printf("osscheck: STREAM accepted=%zu first=%zd calls=%u short=%u\n", sent,first,calls,shorts);
        if (!shorts) {
            fprintf(stderr,"osscheck: FAIL oversized write acknowledged without backpressure\n");
            close(fd); return 1;
        }
    }
    close(fd);
    return 0;
}
