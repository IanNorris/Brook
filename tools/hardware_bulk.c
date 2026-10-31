/* One-shot boot-pinned binary transport. Staging files only: this helper never
 * opens USB boot slots, EFI files or caller-selected guest paths. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../src/shared/update/payload.h"
#include "../src/shared/brook_debug_frame.h"
#include "../src/shared/brook_ahci_export.h"
#define BULK_LIMIT (1024ULL*1024*1024)
static uint64_t deadline_ms;
static int sliding_deadline;
static uint64_t now_ms(void){struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return UINT64_MAX;return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;}
static int valid_id(const char* p){if(strlen(p)!=32)return 0;for(unsigned i=0;i<32;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return 0;return 1;}
static int io_bytes(int fd,void* data,size_t size,int writing,int network){
    uint8_t* p=data;
    while(size){
        if(network){uint64_t now=now_ms();if(now>=deadline_ms)return -1;uint64_t left=deadline_ms-now;int timeout=left>30000?30000:(int)left;
            struct pollfd wait={fd,writing?POLLOUT:POLLIN,0};int n;do{n=poll(&wait,1,timeout);}while(n<0&&errno==EINTR);if(n<=0)return -1;}
        ssize_t n=network?(writing?send(fd,p,size,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,p,size,MSG_DONTWAIT)):(writing?write(fd,p,size):read(fd,p,size));
        if(n<0&&(errno==EINTR||(network&&(errno==EAGAIN||errno==EWOULDBLOCK))))continue;
        if(n<=0)return -1;
        if(network && sliding_deadline)deadline_ms=now_ms()+120000;
        p+=n;size-=(size_t)n;
    }return 0;
}
static int message(int fd,const char* text){return io_bytes(fd,(void*)text,strlen(text),1,1);}
static void hex_hash(const uint8_t hash[32],char text[65]){for(unsigned i=0;i<32;i++)sprintf(text+2*i,"%02x",hash[i]);}
static int hash_fd(int fd,uint64_t bytes,uint8_t hash[32]){
    uint8_t chunk[65536];BrookSha256 context;BrookShaInit(&context);
    if(lseek(fd,0,SEEK_SET)<0)return -1;
    while(bytes){size_t n=bytes>sizeof(chunk)?sizeof(chunk):(size_t)bytes;if(io_bytes(fd,chunk,n,0,0))return -1;BrookShaAdd(&context,chunk,n);bytes-=n;}
    BrookShaFinish(&context,hash);return lseek(fd,0,SEEK_SET)<0?-1:0;
}
static int directory_open(const char* path){
    if(mkdir(path,0755)&&errno!=EEXIST)return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd>=0&&(fstat(fd,&st)||st.st_uid!=geteuid()||(st.st_mode&0022))){close(fd);errno=EPERM;return -1;}
    return fd;
}
static int capture(uint8_t** output,uint64_t* bytes,struct BrookDebugFrameInfo* info){
    long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<0)return -1;
    int result=-1;uint8_t* data=NULL;struct timespec pause={0,10000000};
    for(unsigned i=0;i<500;i++){
        if(syscall(BROOK_SYS_DEBUG_FRAME,1L,token,(long)info,(long)sizeof(*info),0L,0L))goto done;
        if(info->state!=1)break;
        nanosleep(&pause,NULL);
    }
    if(info->state!=2||info->format!=BROOK_DEBUG_FRAME_BGRA8||!info->width||!info->height||
       info->width>4096||info->height>2160||info->stride!=info->width||info->bytes!=(uint64_t)info->width*info->height*4)goto done;
    data=malloc((size_t)info->bytes);if(!data)goto done;
    for(uint64_t offset=0;offset<info->bytes;){
        uint64_t n=info->bytes-offset;if(n>BROOK_DEBUG_FRAME_CHUNK)n=BROOK_DEBUG_FRAME_CHUNK;
        if(syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)(data+offset),(long)n,(long)offset,0L)!=(long)n)goto done;
        offset+=n;
    }
    struct BrookDebugFrameInfo check;
    if(syscall(BROOK_SYS_DEBUG_FRAME,1L,token,(long)&check,(long)sizeof(check),0L,0L)||memcmp(&check,info,sizeof(check)))goto done;
    *output=data;*bytes=info->bytes;data=NULL;result=0;
done:
    free(data);if(syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L)&&!result){free(*output);*output=NULL;result=-1;}
    return result;
}
static int serve(int client,int directory,const char* key,const char* boot,const char* tx,const char* action,const char* source){
    uint8_t header[104];int fd=-1,result=10,partial=0,published=0,saved_errno=0;uint8_t* frame=NULL;
    char temporary[48],final[48];snprintf(temporary,sizeof(temporary),"%s.part",tx);snprintf(final,sizeof(final),"%s.bin",tx);
    if(io_bytes(client,header,sizeof(header),0,1)||memcmp(header,"BRBULK01",8)||
       !BrookUpdateEqual(header+8,(const uint8_t*)key,32)||!BrookUpdateEqual(header+40,(const uint8_t*)boot,32)||
       !BrookUpdateEqual(header+72,(const uint8_t*)tx,32))goto done;
    if(message(client,"{\"state\":\"ready\",\"bulk_api\":1}\n")){result=11;goto done;}
    uint8_t hash[32],meta[40];uint64_t bytes=0;struct BrookDebugFrameInfo info={0};
    if(!strcmp(action,"disk")){
        struct BrookAhciExportInfo disk={0};
        if(syscall(BROOK_SYS_AHCI_EXPORT,0L,(long)&disk,(long)sizeof(disk),0L,0L,0L) ||
           disk.version!=BROOK_AHCI_EXPORT_VERSION || disk.logical_sector_bytes!=512 ||
           !disk.sectors || disk.sectors>UINT64_MAX/512 || !memchr(disk.model,0,sizeof(disk.model)) ||
           !memchr(disk.serial,0,sizeof(disk.serial))){result=28;goto done;}
        bytes=disk.sectors*512;uint8_t identity[154]={0};memcpy(identity,"BRDISK01",8);
        BrookUpdatePut64(identity+8,disk.sectors);BrookUpdatePut32(identity+16,disk.logical_sector_bytes);
        BrookUpdatePut32(identity+20,disk.physical_sector_bytes);BrookUpdatePut32(identity+24,disk.port);
        memcpy(identity+28,disk.model,strnlen(disk.model,sizeof(disk.model)));
        memcpy(identity+69,disk.serial,strnlen(disk.serial,sizeof(disk.serial)));
        memcpy(identity+90,boot,32);memcpy(identity+122,tx,32);
        if(io_bytes(client,identity,sizeof(identity),1,1)){result=29;goto done;}
        uint8_t proceed[8];if(io_bytes(client,proceed,sizeof(proceed),0,1)||memcmp(proceed,"READONLY",8)){result=30;goto done;}
        uint8_t chunk[65536];uint64_t offset=0;BrookSha256 context;BrookShaInit(&context);
        while(offset<bytes){size_t n=bytes-offset>sizeof(chunk)?sizeof(chunk):(size_t)(bytes-offset);
            if(syscall(BROOK_SYS_AHCI_EXPORT,1L,(long)chunk,(long)n,(long)offset,0L,0L)!=(long)n){result=31;goto done;}
            BrookShaAdd(&context,chunk,n);if(io_bytes(client,chunk,n,1,1)){result=32;goto done;}offset+=n;
        }
        struct BrookAhciExportInfo after={0};
        if(syscall(BROOK_SYS_AHCI_EXPORT,0L,(long)&after,(long)sizeof(after),0L,0L,0L) || memcmp(&disk,&after,sizeof(disk))){result=33;goto done;}
        BrookShaFinish(&context,hash);
    }else if(!strcmp(action,"put")){
        if(io_bytes(client,meta,sizeof(meta),0,1)){result=12;goto done;}
        bytes=BrookUpdateGet64(meta);if(bytes>BULK_LIMIT){result=13;goto done;}
        struct statvfs space;
        if(fstatvfs(directory,&space)){result=27;goto done;}
        if(!space.f_frsize||space.f_bavail>UINT64_MAX/space.f_frsize){errno=EOVERFLOW;result=27;goto done;}
        if(bytes>(uint64_t)space.f_bavail*space.f_frsize){errno=ENOSPC;result=27;goto done;}
        struct stat exists;
        if(!fstatat(directory,final,&exists,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT){result=14;goto done;}
        fd=openat(directory,temporary,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);if(fd<0){result=14;goto done;}partial=1;
        uint8_t chunk[65536];uint64_t offset=0;BrookSha256 context;BrookShaInit(&context);
        while(offset<bytes){size_t n=bytes-offset>sizeof(chunk)?sizeof(chunk):(size_t)(bytes-offset);
            if(io_bytes(client,chunk,n,0,1)||io_bytes(fd,chunk,n,1,0)){result=15;goto done;}
            BrookShaAdd(&context,chunk,n);offset+=n;
        }
        BrookShaFinish(&context,hash);
        if(!BrookUpdateEqual(hash,meta+8,32)){result=16;goto done;}
        if(fsync(fd)||hash_fd(fd,bytes,hash)||!BrookUpdateEqual(hash,meta+8,32)){result=17;goto done;}
        if(message(client,"{\"state\":\"staged\",\"published\":false}\n")){result=18;goto done;}
        uint8_t commit[8];if(io_bytes(client,commit,8,0,1)||memcmp(commit,"FINALIZE",8)){result=19;goto done;}
        /* Atomic no-replace publication; another transaction can never
         * replace an existing completed artifact, even under a race. */
        if(syscall(SYS_renameat2,directory,temporary,directory,final,1)){result=20;goto done;}
        partial=0;published=1;
        char published_hash[65];hex_hash(hash,published_hash);
        printf("HWBULK_PUBLISHED {\"transaction_id\":\"%s\",\"published\":true,\"bytes\":%"PRIu64",\"sha256\":\"%s\",\"guest_path\":\"/data/.brook-transfer/%s.bin\"}\n",tx,bytes,published_hash,tx);fflush(stdout);
#ifdef BROOK_BULK_TEST
        if(getenv("BROOK_BULK_TEST_PUBLISHED_FAILURE")){result=21;goto done;}
#endif
    }else if(!strcmp(action,"get")){
        char name[48];snprintf(name,sizeof(name),"%s.bin",source);
        fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
        if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<0||(uint64_t)st.st_size>BULK_LIMIT){result=22;goto done;}
        bytes=(uint64_t)st.st_size;if(hash_fd(fd,bytes,hash)){result=23;goto done;}
    }else{
        if(capture(&frame,&bytes,&info)){result=24;goto done;}
        BrookUpdateHash(frame,(size_t)bytes,hash);
    }
    char text[512],digest[65];hex_hash(hash,digest);
    if(strcmp(action,"put") && strcmp(action,"disk")){
        snprintf(text,sizeof(text),"{\"state\":\"payload\",\"bytes\":%"PRIu64",\"sha256\":\"%s\",\"width\":%u,\"height\":%u,\"format\":%u,\"frame_epoch\":%"PRIu64",\"frame_tick_ms\":%"PRIu64"}\n",bytes,digest,info.width,info.height,info.format,info.epoch,info.tick_ms);
        if(message(client,text)){result=25;goto done;}
        uint8_t chunk[65536];uint64_t offset=0;
        while(offset<bytes){size_t n=bytes-offset>sizeof(chunk)?sizeof(chunk):(size_t)(bytes-offset);
            void* data=frame?(void*)(frame+offset):(void*)chunk;
            if((!frame&&io_bytes(fd,chunk,n,0,0))||io_bytes(client,data,n,1,1)){result=26;goto done;}offset+=n;
        }
    }
    char path[128];
    if(!strcmp(action,"frame") || !strcmp(action,"disk"))strcpy(path,"null");
    else snprintf(path,sizeof(path),"\"/data/.brook-transfer/%s.bin\"",!strcmp(action,"get")?source:tx);
    snprintf(text,sizeof(text),"{\"state\":\"completed\",\"transaction_id\":\"%s\",\"action\":\"%s\",\"bytes\":%"PRIu64",\"sha256\":\"%s\",\"guest_path\":%s,\"retention\":\"%s\"}\n",tx,action,bytes,digest,path,!strcmp(action,"disk")?"not_staged":"current_boot_ram");
    /* Original job receipt disambiguates a lost final TCP reply. */
    printf("HWBULK_COMPLETED %s",text);fflush(stdout);(void)message(client,text);result=0;
done:
    saved_errno=errno;
    free(frame);if(fd>=0)close(fd);if(partial)unlinkat(directory,temporary,0);
    if(result){char error[256];snprintf(error,sizeof(error),"{\"state\":\"failed\",\"code\":%d,\"errno\":%d,\"transaction_id\":\"%s\",\"published\":%s}\n",result,saved_errno,tx,published?"true":"false");(void)message(client,error);}
    return result;
}
int main(int argc,char** argv){
    if((argc!=4&&argc!=5)||!valid_id(argv[1])||!valid_id(argv[2])||
       (strcmp(argv[3],"put")&&strcmp(argv[3],"get")&&strcmp(argv[3],"frame")&&strcmp(argv[3],"disk"))||
       (!strcmp(argv[3],"get")?(argc!=5||!valid_id(argv[4])):argc!=4))return 2;
    const char *root="/data/.brook-transfer",*keypath="/etc/hardware-update-key";unsigned port=1236;
#ifdef BROOK_BULK_TEST
    root=getenv("BROOK_BULK_TEST_ROOT");keypath=getenv("BROOK_BULK_TEST_KEY");const char* p=getenv("BROOK_BULK_TEST_PORT");
    if(!root||!keypath||!p)return 2;
    port=(unsigned)atoi(p);if(!port||port>65535)return 2;
#else
    if(geteuid()!=0)return 3;
    int marker=open("/etc/hardware-maintenance",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(marker<0)return 3;close(marker);
#endif
    int keyfd=open(keypath,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);char key[33]={0},extra;int good=keyfd>=0&&!io_bytes(keyfd,key,32,0,0)&&read(keyfd,&extra,1)==0;
    if(keyfd>=0)close(keyfd);
    if(!good||!valid_id(key))return 3;
    int directory=directory_open(root);if(directory<0)return 4;
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0),client=-1,result=5;
    if(listener<0)goto done;
    int reuse=1;if(setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)))goto done;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr={.s_addr=htonl(INADDR_ANY)}};
    if(bind(listener,(void*)&address,sizeof(address))||listen(listener,1))goto done;
    puts("HWBULK_LISTENING bulk_api=1");fflush(stdout);
    struct pollfd wait={listener,POLLIN,0};if(poll(&wait,1,60000)<=0){result=6;goto done;}
    client=accept(listener,NULL,NULL);if(client<0){result=7;goto done;}
    sliding_deadline=!strcmp(argv[3],"disk");deadline_ms=now_ms()+(sliding_deadline?120000:600000);
    result=serve(client,directory,key,argv[1],argv[2],argv[3],argc==5?argv[4]:NULL);
done:
    if(client>=0)close(client);
    if(listener>=0)close(listener);
    close(directory);
    if(result)fprintf(stderr,"HWBULK_FAILED code=%d\n",result);
    return result;
}
