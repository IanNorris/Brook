/* Exercise patched production callbacks without opening an audio device. */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/soundcard.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include "brook_audio_queue.h"
static int ioctl_error, transfer_error, queued_bytes, write_calls;
static snd_pcm_ioplug_t *queue_io;
static unsigned downstream_frames, charged_override = ~0u;
static unsigned long fail_request;
static ssize_t transferred;
static int fake_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    errno = ioctl_error ? ioctl_error : request == fail_request ? EOPNOTSUPP : 0;
    if (errno) return -1;
    va_list arguments;
    va_start(arguments, request);
    if (request == SNDCTL_DSP_GETODELAY)
        *va_arg(arguments, int*) = queued_bytes;
    else if (request == SNDCTL_DSP_GETOPTR)
        memset(va_arg(arguments, struct count_info*), 0, sizeof(struct count_info));
    else if (request == BROOK_DSP_GETQUEUE) {
        struct BrookAudioQueue *q=va_arg(arguments, struct BrookAudioQueue*);
        unsigned queued=queued_bytes<0?~0u:(unsigned)queued_bytes/4;
        unsigned charged=charged_override==~0u?queued:charged_override;
        *q=(struct BrookAudioQueue){BROOK_AUDIO_QUEUE_VERSION,BROOK_AUDIO_QUEUE_CLOCKED,
            1,queued,0,queue_io->buffer_size,queue_io->buffer_size-charged,
            queued,downstream_frames,44100,queued_bytes%4?0:4};
    }
    va_end(arguments);
    return 0;
}
static ssize_t fake_write(int fd, const void* data, size_t size) {
    (void)fd; (void)data; (void)size;
    ++write_calls;
    errno = transfer_error;
    return transfer_error ? -1 : transferred;
}
static ssize_t fake_read(int fd, void* data, size_t size) {
    return fake_write(fd, data, size);
}
#define ioctl fake_ioctl
#define write fake_write
#define read fake_read
#include "pcm_oss.c"

int main(void) {
    snd_pcm_oss_t oss = {0};
    oss.io.private_data = &oss;
    oss.io.stream = SND_PCM_STREAM_PLAYBACK;
    oss.io.channels = 2;
    oss.io.rate = 44100;
    oss.io.buffer_size = 16;
    queue_io = &oss.io;
    oss.frame_bytes = 4;
    oss.format = AFMT_S16_LE;
    oss.boundary = 1024;
#ifdef BROOK_OSS_DOWNSTREAM
    oss.queue_epoch=1;oss.queue_capacity=16;
#endif
    assert(pthread_mutex_init(&oss.lock, NULL) == 0);
    /* Real ALSA params exercise prepare without any host audio device. */
    assert(snd_pcm_open(&oss.io.pcm, "null", SND_PCM_STREAM_PLAYBACK, 0) == 0);
    assert(snd_pcm_set_params(oss.io.pcm, SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1, 200000) == 0);
    char samples[8] = {0};
    snd_pcm_channel_area_t area = { samples, 0, 32 };
    transfer_error = EAGAIN;
    assert(oss_write(&oss.io, &area, 0, 2) == -EAGAIN);
    assert(oss_read(&oss.io, &area, 0, 2) == -EAGAIN);
    transfer_error = 0;
    ioctl_error = EPIPE;
    assert(oss_pointer(&oss.io) == -EPIPE);
    assert(oss_playback_callback.pointer(&oss.io) == -EPIPE);
    snd_pcm_sframes_t delay = 123;
    assert(oss_delay(&oss.io, &delay) == -EPIPE && delay == 123);
    assert(oss_playback_callback.delay(&oss.io, &delay) == -EPIPE && delay == 123);
    assert(oss_drain(&oss.io) == -EPIPE);
    ioctl_error = ENODEV;
    assert(oss_start(&oss.io) == -ENODEV);
    assert(oss_stop(&oss.io) == -ENODEV);
    assert(oss_prepare(&oss.io) == -ENODEV);
    assert(oss.boundary == 0);
    ioctl_error = 0;
    fail_request = SNDCTL_DSP_GETOPTR;
    assert(oss_prepare(&oss.io) == -EOPNOTSUPP && oss.boundary == 0);
    fail_request = 0;
    assert(oss_prepare(&oss.io) == 0 && oss.boundary > 0 && oss.expected_appl == 0);
    transferred = 4;
    assert(oss_write(&oss.io, &area, 0, 2) == 1);
    oss.io.appl_ptr = 1; // The real ioplug forwards only accepted frames.
    transferred = 0;
    assert(oss_write(&oss.io, &area, 0, 2) == 0);
    assert(oss_start(&oss.io) == 0);
    assert(oss_stop(&oss.io) == 0);
    assert(oss_drain(&oss.io) == 0);
    oss.boundary = 1024;
    oss.io.appl_ptr = oss.expected_appl = 16;
    oss.io.hw_ptr = 0;
    queued_bytes = 0;
    assert(oss_pointer(&oss.io) == 16); // Entire ring completed between queries.
    oss.io.hw_ptr = 16;
    assert(oss_pointer(&oss.io) == 16);
    queued_bytes = -4;
    assert(oss_pointer(&oss.io) == -EPIPE);
    queued_bytes = 3;
    assert(oss_pointer(&oss.io) == -EPIPE);
    queued_bytes = 68;
    assert(oss_pointer(&oss.io) == -EPIPE);
    queued_bytes = 0;
    oss.io.appl_ptr = 17; // Software-only forward cannot manufacture completion.
    int before = write_calls;
    assert(oss_pointer(&oss.io) == -EPIPE);
    assert(oss_write(&oss.io, &area, 0, 2) == -EPIPE && write_calls == before);
    oss.io.appl_ptr = oss.io.hw_ptr = 0;
    queued_bytes = 4;
    assert(oss_pointer(&oss.io) == -EPIPE); // Pending-data reset requires prepare.
    queued_bytes = 0;
    assert(oss_pointer(&oss.io) == 0 && oss.expected_appl == 0); // Drained reset.
    oss.expected_appl = oss.io.appl_ptr = 1;
    oss.io.hw_ptr = 1023;
    assert(oss_pointer(&oss.io) == 1); // Actual ALSA boundary wrap.
#ifdef BROOK_OSS_DOWNSTREAM
    /* Reusable space is not a drained device. An old origin cannot rebase
     * while only downstream samples remain, even with zero charged occupancy. */
    oss.queue_staging=downstream_frames=16;
    oss.expected_appl=1;oss.io.appl_ptr=oss.io.hw_ptr=0;
    charged_override=0;queued_bytes=32;
    assert(oss_pointer(&oss.io)==-EPIPE);
    oss.io.appl_ptr=oss.expected_appl=16;
    assert(oss_pointer(&oss.io)==16);
    assert(oss_delay(&oss.io,&delay)==0 && delay==8);
    /* Full client occupancy plus downstream exceeds the logical ring; delay
     * remains physical while the reusable cursor stays at the old origin. */
    oss.io.hw_ptr=0;charged_override=16;queued_bytes=128;
    assert(oss_pointer(&oss.io)==0);
    assert(oss_delay(&oss.io,&delay)==0 && delay==32);
    ++oss.queue_epoch;assert(oss_pointer(&oss.io)==-EPIPE);--oss.queue_epoch;
    ++oss.queue_capacity;assert(oss_pointer(&oss.io)==-EPIPE);--oss.queue_capacity;
    queued_bytes=132;assert(oss_pointer(&oss.io)==-EPIPE); // >client+staging
    /* A device copy can own more than the client ring, without advancing
     * physical completion or admitting more than one logical ring. */
    oss.queue_staging=downstream_frames=4096;
    charged_override=0;queued_bytes=4096*4;
    assert(oss_pointer(&oss.io)==16);
    assert(oss_delay(&oss.io,&delay)==0 && delay==4096);
    charged_override=16;queued_bytes=(4096+16)*4;
    assert(oss_pointer(&oss.io)==0);
    assert(oss_delay(&oss.io,&delay)==0 && delay==4112);
    queued_bytes+=4;assert(oss_pointer(&oss.io)==-EPIPE);
    oss.queue_staging=downstream_frames=4097;
    queued_bytes=0;charged_override=0;
    assert(oss_pointer(&oss.io)==-EPIPE);
#endif
    assert(snd_pcm_close(oss.io.pcm) == 0);
    assert(pthread_mutex_destroy(&oss.lock) == 0);
    puts("OSS_CALLBACK_ERRORS PASS");
    return 0;
}
