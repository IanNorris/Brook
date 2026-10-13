// Live ABI probe: request numbers come from the toolchain's Linux headers.
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

int main(void)
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
    close(fd);
    return 0;
}
