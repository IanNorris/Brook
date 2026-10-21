/* Real production plugin/ALSA negotiation with a synthetic OSS endpoint.
 * The only opened device is /dev/null; ioctl never reaches the host. */
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
static unsigned physical_fragments = 256, physical_fragment_bytes = 256;
static int rate_test_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list arguments;
    va_start(arguments, request);
    if (request == SNDCTL_DSP_GETCAPS) *va_arg(arguments, int *) = DSP_CAP_TRIGGER;
    else if (request == SNDCTL_DSP_GETFMTS) *va_arg(arguments, int *) = AFMT_S16_LE;
    else if (request == SNDCTL_DSP_GETODELAY) *va_arg(arguments, int *) = 0;
    else if (request == SNDCTL_DSP_GETOPTR)
        memset(va_arg(arguments, struct count_info *), 0, sizeof(struct count_info));
    else if (request == SNDCTL_DSP_GETOSPACE) {
        struct audio_buf_info *space = va_arg(arguments, struct audio_buf_info *);
        memset(space, 0, sizeof(*space));
        space->fragstotal = physical_fragments; space->fragsize = physical_fragment_bytes;
    }
    else if (request == SNDCTL_DSP_SETFRAGMENT) {
        unsigned value = (unsigned)*va_arg(arguments, int *);
        unsigned exponent = value & 0xffff, count = value >> 16;
        /* Match Brook's physical endpoint bounds, not a permissive null PCM. */
        if (exponent < 8 || exponent > 16 || !count ||
            (unsigned long long)count * (1U << exponent) > 65536) {
            va_end(arguments);errno = EINVAL;return -1;
        }
        physical_fragments = count; physical_fragment_bytes = 1U << exponent;
    }
    va_end(arguments);
    return 0;
}
#define ioctl rate_test_ioctl
#include "pcm_oss.c"
