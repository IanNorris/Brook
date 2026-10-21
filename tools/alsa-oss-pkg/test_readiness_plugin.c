/* Host-only production-plugin fixture. All device operations are synthetic.
 * The actual poll FD is an eventfd: POLLOUT is level-ready exactly when
 * Brook's configured writable threshold would be ready. No host audio device
 * is opened.
 */
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
static int signal_fd=-1;
static uint64_t submitted,completed,capacity=16384,fragment=64,watermark=64,ioctls,writes;
static int fail_low_water;
static void sync_level(void) {
    uint64_t value;
    /* The plugin may clear O_NONBLOCK on its dup, sharing the same OFD.
     * Only read a counter known to be nonzero. This fixture is single-threaded.
     */
    struct pollfd descriptor={.fd=signal_fd,.events=POLLIN};
    int ready=poll(&descriptor,1,0);assert(ready>=0);
    if(ready && (descriptor.revents&POLLIN))
        assert(read(signal_fd,&value,sizeof(value))==sizeof(value));
    if (capacity-(submitted-completed)<watermark) {
        value=UINT64_MAX-1;
        assert(write(signal_fd,&value,sizeof(value))==sizeof(value));
    }
}
static int readiness_open(const char *path,int flags,...) {
    (void)flags;
    assert(strcmp(path,"/brook-synthetic-readiness")==0);
    if(signal_fd<0)signal_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    assert(signal_fd>=0);
    submitted=completed=ioctls=writes=0;capacity=16384;fragment=watermark=64;
    sync_level();return dup(signal_fd);
}
static int readiness_ioctl(int fd,unsigned long request,...) {
    (void)fd;++ioctls;
    if(request==SNDCTL_DSP_RESET) {submitted=completed=0;sync_level();return 0;}
    if(request==SNDCTL_DSP_SYNC) {completed=submitted;sync_level();return 0;}
    va_list args;va_start(args,request);
    void *argument=va_arg(args,void*);va_end(args);
    if(request==0x40045022UL) {
        if(fail_low_water){errno=ENOTTY;return -1;}
        int bytes=*(int*)argument;
        if(bytes<=0 || bytes%4 || (uint64_t)bytes/4>capacity){errno=EINVAL;return -1;}
        watermark=bytes/4;sync_level();
    } else if(request==SNDCTL_DSP_GETCAPS) *(int*)argument=DSP_CAP_TRIGGER;
    else if(request==SNDCTL_DSP_GETFMTS) *(int*)argument=AFMT_S16_LE;
    else if(request==SNDCTL_DSP_GETODELAY) *(int*)argument=(submitted-completed)*4;
    else if(request==SNDCTL_DSP_GETOPTR) {
        struct count_info *p=argument;
        p->bytes=completed*4;p->blocks=(submitted-completed)/fragment;p->ptr=(completed%capacity)*4;
    } else if(request==SNDCTL_DSP_SETFRAGMENT) {
        unsigned value=*(unsigned*)argument,exponent=value&65535,count=value>>16;
        assert(exponent>=8 && exponent<=16 && count && (uint64_t)count*(1u<<exponent)<=65536);
        assert(submitted==completed);fragment=(1u<<exponent)/4;capacity=count*fragment;watermark=fragment;sync_level();
    } else if(request==SNDCTL_DSP_GETBLKSIZE) *(int*)argument=fragment*4;
    else if(request==SNDCTL_DSP_GETOSPACE) {
        struct audio_buf_info *p=argument;
        p->bytes=(capacity-(submitted-completed))*4;p->fragsize=fragment*4;
        p->fragments=p->bytes/p->fragsize;p->fragstotal=capacity/fragment;
    } else if(request==SNDCTL_DSP_SPEED) {assert(*(int*)argument==44100);}
    else if(request==SNDCTL_DSP_CHANNELS) {assert(*(int*)argument==2);}
    else if(request==SNDCTL_DSP_SETFMT) {assert(*(int*)argument==AFMT_S16_LE);}
    else if(request==SNDCTL_DSP_SETTRIGGER) {assert((*(int*)argument&~2)==0);}
    else {errno=EOPNOTSUPP;return -1;}
    return 0;
}
static ssize_t readiness_write(int fd,const void *data,size_t bytes) {
    (void)fd;(void)data;++writes;assert(bytes%4==0);
    uint64_t frames=bytes/4,available=capacity-(submitted-completed);
    if(frames>available)frames=available;
    if(!frames && bytes){errno=EAGAIN;return -1;}
    submitted+=frames;sync_level();return frames*4;
}
void brook_readiness_advance(unsigned frames) {
    uint64_t queued=submitted-completed;if(frames>queued)frames=queued;
    completed+=frames;sync_level();
}
uint64_t brook_readiness_submitted(void){return submitted;}
uint64_t brook_readiness_completed(void){return completed;}
uint64_t brook_readiness_capacity(void){return capacity;}
uint64_t brook_readiness_fragment(void){return fragment;}
uint64_t brook_readiness_ioctls(void){return ioctls;}
uint64_t brook_readiness_writes(void){return writes;}
uint64_t brook_readiness_watermark(void){return watermark;}
void brook_readiness_fail_watermark(int fail){fail_low_water=fail;}
#define open readiness_open
#define ioctl readiness_ioctl
#define write readiness_write
#include "pcm_oss.c"
