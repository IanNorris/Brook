/* Bounded host reproduction: real ALSA/plugin/default revents and real poll;
 * only device progress and fragment-level readiness are synthetic.
 */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
static void ok(int error){if(error<0){fprintf(stderr,"ALSA: %s\n",snd_strerror(error));abort();}}
typedef uint64_t (*get_fn)(void);
int main(int argc,char **argv) {
    alarm(10);setvbuf(stdout,NULL,_IOLBF,0);
    assert(argc==3 || argc==4);unsigned rate=strtoul(argv[2],0,10);assert(rate==8000 || rate==22050 || rate==44100 || rate==48000 || rate==96000);
    void *module=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);assert(module);
    void (*advance)(unsigned)=dlsym(module,"brook_readiness_advance");
    get_fn capacity=dlsym(module,"brook_readiness_capacity"),fragment=dlsym(module,"brook_readiness_fragment"),
           submitted=dlsym(module,"brook_readiness_submitted"),ioctls=dlsym(module,"brook_readiness_ioctls"),writes=dlsym(module,"brook_readiness_writes"),watermark=dlsym(module,"brook_readiness_watermark");
    void (*fail_watermark)(int)=dlsym(module,"brook_readiness_fail_watermark");
    assert(advance && capacity && fragment && submitted && ioctls && writes && watermark && fail_watermark);
    char text[4096];int length=snprintf(text,sizeof(text),
      "pcm_type.oss { lib \"%s\" }\n"
      "pcm.test { type plug slave { pcm { type oss device /brook-synthetic-readiness } format S16_LE rate 44100 channels 2 } }\n",argv[1]);
    assert(length>0 && length<(int)sizeof(text));
    snd_config_t *config;snd_input_t *input;snd_pcm_t *pcm;
    ok(snd_config_top(&config));ok(snd_input_buffer_open(&input,text,length));ok(snd_config_load(config,input));ok(snd_input_close(input));
    ok(snd_pcm_open_lconf(&pcm,"test",SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK,config));
    ok(snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,rate,1,100000));
    snd_pcm_uframes_t buffer,period;ok(snd_pcm_get_params(pcm,&buffer,&period));
    snd_pcm_sw_params_t *sw;snd_pcm_sw_params_alloca(&sw);ok(snd_pcm_sw_params_current(pcm,sw));
    ok(snd_pcm_sw_params_set_avail_min(pcm,sw,period));ok(snd_pcm_sw_params_set_start_threshold(pcm,sw,buffer+1));ok(snd_pcm_sw_params(pcm,sw));
    snd_output_t *dump;ok(snd_output_stdio_attach(&dump,stdout,0));ok(snd_pcm_dump(pcm,dump));ok(snd_output_close(dump));
    short *samples=calloc(buffer*2,sizeof(short));assert(samples);
    snd_pcm_uframes_t initial=argc==4?buffer-1:buffer;
    snd_pcm_sframes_t written=snd_pcm_writei(pcm,samples,initial);assert(written==(snd_pcm_sframes_t)initial);ok(snd_pcm_start(pcm));
    int count=snd_pcm_poll_descriptors_count(pcm);assert(count>0 && count<=8);struct pollfd fds[8];ok(snd_pcm_poll_descriptors(pcm,fds,count));
    /* Optional stronger gate: a converted producer may retain a partial
     * client period above the native PCM. A static slave threshold does not
     * observe that state. Return failure, not an expected-pass bug assertion,
     * if raw readiness can still spin below the client's minimum. */
    if(argc==4) {
        for(unsigned step=0;step<16384;++step) {
            if(poll(fds,count,0)>0) {
                snd_pcm_sframes_t available=snd_pcm_avail_update(pcm);
                assert(available>=0);
                if(available<(snd_pcm_sframes_t)period) {
                    printf("RAW_READINESS_COUNTEREXAMPLE rate=%u initial=%lu native_progress=%u available=%ld period=%lu watermark=%lu\n",rate,initial,step,available,period,watermark());
                    ok(snd_pcm_drop(pcm));ok(snd_pcm_close(pcm));ok(snd_config_delete(config));free(samples);dlclose(module);
                    return 1;
                }
                break;
            }
            advance(1);
        }
        ok(snd_pcm_drop(pcm));ok(snd_pcm_close(pcm));ok(snd_config_delete(config));free(samples);dlclose(module);
        return 0;
    }
    snd_pcm_sframes_t before=snd_pcm_avail_update(pcm);assert(before>=0 && before<(snd_pcm_sframes_t)period);
    int raw_before=poll(fds,count,0);assert(raw_before==0);
    uint64_t initial_ioctls=ioctls(),initial_writes=writes(),initial_submitted=submitted();
    /* Hold playback progress after one physical fragment has completed. */
    advance(fragment());unsigned raw_ready=0,alsa_ready=0,insufficient=0;
    snd_pcm_sframes_t available=0;
    for(unsigned i=0;i<128;++i) {
        for(int j=0;j<count;++j)fds[j].revents=0;
        int ready=poll(fds,count,0);assert(ready>=0);raw_ready+=ready>0;
        unsigned short events=0;ok(snd_pcm_poll_descriptors_revents(pcm,fds,count,&events));
        alsa_ready+=(events&POLLOUT)!=0;
        available=snd_pcm_avail_update(pcm);assert(available>=0);
        if(available<(snd_pcm_sframes_t)period)++insufficient;
    }
    printf("READINESS_REPRO rate=%u buffer=%lu period=%lu physical_capacity=%lu fragment=%lu available=%ld raw_ready=%u alsa_ready=%u insufficient=%u pointer_ioctls=%lu transfers=%lu frames_added=%lu\n",rate,buffer,period,capacity(),fragment(),available,raw_ready,alsa_ready,insufficient,ioctls()-initial_ioctls,writes()-initial_writes,submitted()-initial_submitted);
    assert(raw_ready==0 && alsa_ready==0 && insufficient==128);
    assert(poll(fds,count,5)==0);
    assert(writes()==initial_writes && submitted()==initial_submitted);
    /* The physical threshold includes rounded spare ring capacity. At the
     * exact threshold raw poll must become ready; no simulated time elapses
     * between these two adjacent native-frame positions. */
    uint64_t queued=submitted()-fragment();
    uint64_t free_frames=capacity()-queued;
    assert(watermark()>free_frames);
    advance(watermark()-free_frames-1);assert(poll(fds,count,0)==0);
    advance(1);assert(poll(fds,count,0)>0);
    available=snd_pcm_avail_update(pcm);printf("READINESS_BOUNDARY rate=%u watermark=%lu available=%ld period=%lu\n",rate,watermark(),available,period);assert(available>=(snd_pcm_sframes_t)period);
    printf("READINESS_THRESHOLD rate=%u watermark=%lu logical_available=%ld period=%lu\n",rate,watermark(),available,period);
    uint64_t old_watermark=watermark(),old_submitted=submitted();
    ok(snd_pcm_sw_params_current(pcm,sw));
    ok(snd_pcm_sw_params_set_avail_min(pcm,sw,period*2));ok(snd_pcm_sw_params(pcm,sw));
    assert(watermark()>old_watermark && poll(fds,count,0)==0 && submitted()==old_submitted);
    ok(snd_pcm_sw_params_set_avail_min(pcm,sw,period));ok(snd_pcm_sw_params(pcm,sw));
    assert(watermark()==old_watermark && poll(fds,count,0)>0 && submitted()==old_submitted);
    /* Unsupported endpoints must fail rather than silently spin. */
    fail_watermark(1);assert(snd_pcm_sw_params(pcm,sw)==-ENOTTY);fail_watermark(0);
    ok(snd_pcm_sw_params(pcm,sw));
    written=snd_pcm_writei(pcm,samples,period);assert(written==(snd_pcm_sframes_t)period);
    ok(snd_pcm_drop(pcm));ok(snd_pcm_prepare(pcm));
    assert(watermark()==old_watermark);
    written=snd_pcm_writei(pcm,samples,buffer);assert(written==(snd_pcm_sframes_t)buffer);ok(snd_pcm_start(pcm));
    assert(poll(fds,count,0)==0);
    printf("READINESS_CONTROL rate=%u dynamic_avail_min=pass unsupported=pass reprepare=pass\n",rate);
    ok(snd_pcm_drop(pcm));ok(snd_pcm_close(pcm));ok(snd_config_delete(config));free(samples);dlclose(module);
    return 0;
}
