#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>
int main(void){
 int fd=open("/dev/dsp",O_WRONLY);
 if(fd<0){perror("open dsp");return 1;}
 int rate=44100,channels=2,format=AFMT_S16_LE,delay=-1;
 if(ioctl(fd,SNDCTL_DSP_SPEED,&rate)||ioctl(fd,SNDCTL_DSP_CHANNELS,&channels)||ioctl(fd,SNDCTL_DSP_SETFMT,&format))return 1;
 short silence[4096]={0};
 if(write(fd,silence,sizeof(silence))!=sizeof(silence))return 1;
 if(ioctl(fd,SNDCTL_DSP_GETODELAY,&delay))return 1;
 usleep(750000);int later=-1;
 if(ioctl(fd,SNDCTL_DSP_GETODELAY,&later))return 1;
 close(fd);
 int good=delay>0&&delay<44100*4*2&&later>=0&&later<delay;
 printf("DELAYCHECK %s: queued=%d bytes; after 750ms=%d bytes\n",good?"PASS":"FAIL",delay,later);
 return !good;
}
