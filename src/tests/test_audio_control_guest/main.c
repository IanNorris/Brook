#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../../shared/brook_audio_control.h"
static int fail(const char *why){printf("AUDIO_CONTROL_FAIL %s errno=%d\n",why,errno);return 1;}
static int configure(int fd,unsigned long command,int wanted){int actual=wanted;return ioctl(fd,command,&actual)==0 && actual==wanted;}
int main(int argc,char **argv){
    setvbuf(stdout,NULL,_IONBF,0);
    if(syscall(BROOK_SYS_AUDIO_CONTROL,0L,0L)!=BROOK_AUDIO_PROBE_DEFAULT)return fail("not muted at boot");
    if(syscall(BROOK_SYS_AUDIO_CONTROL,1L,101L)!=-1 || errno!=EINVAL)return fail("invalid volume accepted");
    if(syscall(BROOK_SYS_AUDIO_CONTROL,0L,0L)!=BROOK_AUDIO_PROBE_DEFAULT)return fail("invalid changed level");
    const int available=syscall(BROOK_SYS_AUDIO_CONTROL,2L,0L);
    if(argc>1 && !strcmp(argv[1],"no-device")){
        if(available!=0)return fail("unexpected device");
        puts("AUDIO_CONTROL_NO_DEVICE_OK");return 0;
    }
    if(available!=1)return fail("device missing");
    int allowLegacyDrain=0,toneOnly=0;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"legacy-drain"))allowLegacyDrain=1;
        else if(!strcmp(argv[i],"tone-only"))toneOnly=1;
        else return fail("unknown option");
    }
    int fd=open("/dev/dsp",O_WRONLY);if(fd<0)return fail("open dsp");
    if(!configure(fd,0xC0045005,16) || !configure(fd,0xC0045006,2) || !configure(fd,0xC0045002,44100))return fail("PCM configuration");
    static int16_t samples[2048];
    const unsigned levels[]={BROOK_AUDIO_PROBE_DEFAULT,25,50,100,BROOK_AUDIO_MUTED|75,25};
    const unsigned phases=toneOnly?1:sizeof(levels)/sizeof(levels[0]);
    for(unsigned phase=0;phase<phases;++phase){
        const unsigned level=toneOnly?100:levels[phase];
        if(syscall(BROOK_SYS_AUDIO_CONTROL,1L,(long)level)!=0)return fail("set");
        if(syscall(BROOK_SYS_AUDIO_CONTROL,0L,0L)!=(long)level)return fail("query");
        printf("AUDIO_CONTROL_PHASE %u output=%u\n",phase,level);
        for(unsigned chunk=0;chunk<44;++chunk){
            for(unsigned i=0;i<1024;++i)samples[i*2]=samples[i*2+1]=(((chunk*1024+i)/50)&1)?8000:-8000;
            unsigned done=0;while(done<sizeof(samples)){
                ssize_t n=write(fd,(const char*)samples+done,sizeof(samples)-done);
                if(n<=0)return fail("write");
                done+=(unsigned)n;
            }
        }
        if(ioctl(fd,0x5001,0)<0) {
            if(!allowLegacyDrain || errno!=ENOTSUP)return fail("drain");
            // The TCG fixture can lack a trusted elapsed DMA clock. Allow the
            // buffered legacy path time to empty; the host WAV proves output.
            if(usleep(1000000)<0)return fail("legacy drain wait");
        }
        usleep(250000);
    }
    close(fd);
    if(syscall(BROOK_SYS_AUDIO_CONTROL,1L,(long)BROOK_AUDIO_PROBE_DEFAULT)!=0)return fail("restore mute");
    puts("AUDIO_CONTROL_OK");return 0;
}
