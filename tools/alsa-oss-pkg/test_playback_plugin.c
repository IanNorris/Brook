/* Stateful fake OSS device under the actual production ioplug. Only /dev/null
 * is opened. Completion advances explicitly, independently of ALSA queries.
 * These two physical geometries model the native Firefox comparison. */
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

static unsigned capacity_frames, fragment_frames, queued_frames, frame_bytes = 4;
static unsigned geometry;
static int triggered;
static unsigned long long accepted_frames, completed_frames, write_calls;
static unsigned long long max_request_frames, logical_overfills;
static snd_pcm_ioplug_t *observed_io;

void playback_geometry(unsigned enlarged) {
    geometry = enlarged;
    capacity_frames = fragment_frames = queued_frames = 0;
    accepted_frames = completed_frames = write_calls = 0;
    max_request_frames = logical_overfills = 0;
    observed_io = NULL;
    triggered = 0;
}
void playback_advance(unsigned frames) {
    if (!triggered) return;
    if (frames > queued_frames) frames = queued_frames;
    queued_frames -= frames;
    completed_frames += frames;
}
unsigned long long playback_stat(unsigned key) {
    switch (key) {
    case 0: return capacity_frames;
    case 1: return fragment_frames;
    case 2: return queued_frames;
    case 3: return accepted_frames;
    case 4: return completed_frames;
    case 5: return write_calls;
    case 6: return max_request_frames;
    case 7: return logical_overfills;
    case 8: return observed_io ? observed_io->buffer_size : 0;
    default: assert(0); return 0;
    }
}
static int playback_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list arguments;
    va_start(arguments, request);
    if (request == SNDCTL_DSP_GETCAPS) *va_arg(arguments, int *) = DSP_CAP_TRIGGER;
    else if (request == SNDCTL_DSP_GETFMTS) *va_arg(arguments, int *) = AFMT_S16_LE;
    else if (request == SNDCTL_DSP_GETODELAY)
        *va_arg(arguments, int *) = queued_frames * frame_bytes;
    else if (request == SNDCTL_DSP_GETOPTR) {
        struct count_info *p = va_arg(arguments, struct count_info *);
        memset(p, 0, sizeof(*p));
        p->bytes = completed_frames * frame_bytes;
        p->ptr = capacity_frames ? p->bytes % (capacity_frames * frame_bytes) : 0;
    } else if (request == SNDCTL_DSP_GETOSPACE) {
        struct audio_buf_info *space = va_arg(arguments, struct audio_buf_info *);
        memset(space, 0, sizeof(*space));
        space->fragstotal = capacity_frames / fragment_frames;
        space->fragsize = fragment_frames * frame_bytes;
        space->bytes = (capacity_frames - queued_frames) * frame_bytes;
        space->fragments = space->bytes / space->fragsize;
    } else if (request == SNDCTL_DSP_SETFRAGMENT) {
        unsigned value = *va_arg(arguments, int *);
        unsigned count = value >> 16, exponent = value & 0xffff;
        assert(count && exponent >= 8 && exponent <= 16);
        capacity_frames = count * (1U << exponent) / frame_bytes;
        fragment_frames = (1U << exponent) / frame_bytes;
        /* The regression control uses a 4410-frame logical ring. Other ring
         * sizes keep the ordinary rounded-up geometry, not a guessed model. */
        if (geometry && capacity_frames == 4416 && fragment_frames == 64) {
            capacity_frames = 6016;
            fragment_frames = 128;
        }
    } else if (request == SNDCTL_DSP_RESET) {
        queued_frames = 0;
        accepted_frames = completed_frames = 0;
        triggered = 0;
    } else if (request == SNDCTL_DSP_SETTRIGGER) {
        int value = *va_arg(arguments, int *);
        assert((value & ~PCM_ENABLE_OUTPUT) == 0);
        triggered = (value & PCM_ENABLE_OUTPUT) != 0;
    } else if (request == SNDCTL_DSP_SYNC) {
        completed_frames += queued_frames;
        queued_frames = 0;
    }
    va_end(arguments);
    return 0;
}
static ssize_t playback_write(int fd, const void *data, size_t bytes) {
    (void)fd; (void)data;
    assert(bytes % frame_bytes == 0);
    unsigned long long frames = bytes / frame_bytes;
    ++write_calls;
    if (frames > max_request_frames) max_request_frames = frames;
    if (frames > capacity_frames - queued_frames) frames = capacity_frames - queued_frames;
    if (!frames) { errno = EAGAIN; return -1; }
    queued_frames += frames;
    accepted_frames += frames;
    if (observed_io && queued_frames > observed_io->buffer_size) ++logical_overfills;
    return frames * frame_bytes;
}
/* Observe parameters without replacing any production callbacks. */
static int playback_create(snd_pcm_ioplug_t *io, const char *name,
    snd_pcm_stream_t stream, int mode) {
    observed_io = io;
    return snd_pcm_ioplug_create(io, name, stream, mode);
}
#define snd_pcm_ioplug_create playback_create
#define ioctl playback_ioctl
#define write playback_write
#include "pcm_oss.c"
