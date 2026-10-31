/* Opt-in, one-shot, fixed destination USB store provisioning. */
#include "hardware_store_transaction.h"
#include "../src/kernel/src/writable_desktop_plan.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static bool hex32(const char *s){if(!s||strlen(s)!=32)return false;for(int i=0;i<32;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;return true;}
static uint64_t now_ms(){timespec t{};return clock_gettime(CLOCK_MONOTONIC,&t)?UINT64_MAX:uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
static bool receive(int fd,void *buf,size_t length,uint64_t deadline){auto *p=static_cast<uint8_t*>(buf);while(length){uint64_t now=now_ms();if(now>=deadline)return false;int ms=deadline-now>60000?60000:int(deadline-now);pollfd poller{fd,POLLIN,0};int r=poll(&poller,1,ms);if(r<0&&errno==EINTR)continue;if(r<=0)return false;ssize_t n=recv(fd,p,length,MSG_DONTWAIT);if(n<0&&(errno==EINTR||errno==EAGAIN))continue;if(n<=0)return false;p+=n;length-=size_t(n);}return true;}
static bool send_text(int fd,const char *text,uint64_t deadline){size_t left=strlen(text);while(left){uint64_t now=now_ms();if(now>=deadline)return false;int ms=deadline-now>60000?60000:int(deadline-now);pollfd p{fd,POLLOUT,0};int r=poll(&p,1,ms);if(r<0&&errno==EINTR)continue;if(r<=0)return false;ssize_t n=send(fd,text,left,MSG_NOSIGNAL|MSG_DONTWAIT);if(n<0&&(errno==EINTR||errno==EAGAIN))continue;if(n<=0)return false;text+=n;left-=size_t(n);}return true;}
static bool destination_valid(int dir,uint64_t bytes){struct statfs fs{};struct statvfs v{};
    if(fstatfs(dir,&fs)||fstatvfs(dir,&v)||!v.f_frsize||v.f_bavail>UINT64_MAX/v.f_frsize)return false;
#ifndef BROOK_STORE_RECEIVER_TEST
    if(static_cast<uint64_t>(fs.f_type)!=0x4d44)return false;
    // The initrd itself is FAT too. The USB mount must be distinct from its
    // parent, otherwise a missing mount could target the RAM-backed directory.
    struct stat target{},parent{};
    if(fstat(dir,&target)||stat("/media",&parent)||target.st_dev==parent.st_dev)return false;
#endif
    return !(v.f_flag&ST_RDONLY) && bytes<=uint64_t(v.f_bavail)*v.f_frsize;
}
static int serve(int client,int dir,const char *key,const char *boot,const char *tx,bool *modified){
    const uint64_t deadline=now_ms()+4ULL*60*60*1000;uint8_t header[104];
    if(!receive(client,header,sizeof(header),deadline)||memcmp(header,"BRSTORE1",8)||!BrookUpdateEqual(header+8,(const uint8_t*)key,32)||!BrookUpdateEqual(header+40,(const uint8_t*)boot,32)||!BrookUpdateEqual(header+72,(const uint8_t*)tx,32))return 10;
    if(!send_text(client,"{\"state\":\"ready\",\"store_api\":1}\n",deadline))return 11;
    uint8_t meta[36];if(!receive(client,meta,sizeof(meta),deadline))return 12;
    uint32_t length=BrookUpdateGet32(meta);if(!length||length>96)return 13;
    char record[97]{};uint8_t begin[8];if(!receive(client,record,length,deadline)||!receive(client,begin,8,deadline)||memcmp(begin,"BEGIN001",8))return 14;
    brook::WritableDesktopPlan plan{};if(!brook::WritableDesktopPlanParse(record,length,plan))return 15;
    if(!destination_valid(dir,plan.bytes))return 23;
    HardwareStoreTransaction transaction;int e=transaction.begin(dir,record,length,meta+4);*modified=transaction.image_created();if(e)return 16;
    if(!send_text(client,"{\"state\":\"receiving\"}\n",deadline))return 17;
    uint8_t chunk[65536];uint64_t left=plan.bytes;
    while(left){size_t n=left>sizeof(chunk)?sizeof(chunk):size_t(left);if(!receive(client,chunk,n,deadline)||transaction.append(chunk,n))return 18;left-=n;}
    if(transaction.stage())return 19;
    if(!send_text(client,"{\"state\":\"staged\",\"published\":false}\n",deadline))return 20;
    uint8_t finalize[8];if(!receive(client,finalize,8,deadline)||memcmp(finalize,"FINALIZE",8))return 21;
    e=transaction.finalize();
    printf("HWSTORE_RESULT transaction=%s published=%u durable=%u error=%d\n",tx,transaction.marker_published()?1:0,transaction.marker_durable()?1:0,e);fflush(stdout);
    if(e){char message[160];snprintf(message,sizeof(message),"{\"state\":\"failed\",\"code\":22,\"published\":%s,\"durable\":false}\n",transaction.marker_published()?"true":"false");send_text(client,message,deadline);return 22;}
    send_text(client,"{\"state\":\"completed\",\"published\":true,\"durable\":true}\n",deadline);return 0;
}
int main(int argc,char **argv){
    if(argc!=3||!hex32(argv[1])||!hex32(argv[2]))return 2;
    const char *root="/media/usb",*keypath="/etc/hardware-update-key",*marker="/etc/hardware-maintenance",*lock="/tmp/hardware-update.lock";unsigned port=1237;
#ifdef BROOK_STORE_RECEIVER_TEST
    root=getenv("BROOK_STORE_ROOT");keypath=getenv("BROOK_STORE_KEY");marker=keypath;lock=getenv("BROOK_STORE_LOCK");const char *port_text=getenv("BROOK_STORE_PORT");if(!root||!keypath||!lock||!port_text)return 2;port=unsigned(atoi(port_text));if(!port||port>65535)return 2;
#else
    if(geteuid()!=0)return 3;
#endif
    int fd=open(marker,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 3;close(fd);
    fd=open(keypath,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 3;char key[33]{};ssize_t n=read(fd,key,33);close(fd);if(n!=32||!hex32(key))return 3;
    fd=open(lock,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 4;if(close(fd))return 4;
    bool modified=false;int result=5,dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),listener=-1,client=-1;
    if(dir>=0){listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener>=0){int reuse=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(uint16_t(port));a.sin_addr.s_addr=htonl(INADDR_ANY);
        if(!bind(listener,(sockaddr*)&a,sizeof(a))&&!listen(listener,1)){puts("HWSTORE_LISTENING store_api=1");fflush(stdout);pollfd p{listener,POLLIN,0};if(poll(&p,1,60000)>0&&(client=accept(listener,nullptr,nullptr))>=0){result=serve(client,dir,key,argv[1],argv[2],&modified);if(result && result!=22){char message[160];snprintf(message,sizeof(message),"{\"state\":\"failed\",\"code\":%d,\"published\":false}\n",result);send_text(client,message,now_ms()+60000);}}}}}
    if(client>=0)close(client);if(listener>=0)close(listener);if(dir>=0)close(dir);
    if(!result||!modified)unlink(lock);if(result)fprintf(stderr,"HWSTORE_FAILED code=%d image_created=%u\n",result,modified?1:0);return result;
}
