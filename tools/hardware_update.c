/* Opt-in RAM maintenance uploader. Fixed preprovisioned files only; no shell,
 * arbitrary paths, creation, truncation, bootloader writes or automatic reset.
 * Bulk bytes use a separate socket, never the line debug-command parser. */
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
#include <unistd.h>
#include "../src/shared/update/payload.h"
#include "../src/shared/update/executable.h"
static const char* names[6]={"A/KERNEL.ELF","A/INITRD.IMG","B/KERNEL.ELF","B/INITRD.IMG","SELECT0.BIN","SELECT1.BIN"};
static int directory=-1;
static unsigned mutated;
static int open_existing(unsigned index) {
    int fd=openat(directory,names[index],O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -1;
    struct stat st;uint64_t want=index>=4?128:(index%2?BROOK_UPDATE_INITRD_MAX:BROOK_UPDATE_KERNEL_MAX);
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||(uint64_t)st.st_size!=want){close(fd);errno=EINVAL;return -1;}
    return fd;
}
static int file_read(int fd,uint8_t* p,size_t n) {
    while(n){ssize_t got=read(fd,p,n);if(got<0&&errno==EINTR)continue;if(got<=0)return -1;p+=got;n-=(size_t)got;}return 0;
}
static int hash_file(unsigned index,size_t length,uint8_t out[32]) {
    int fd=open_existing(index);if(fd<0)return -1;
    BrookSha256 hash;BrookShaInit(&hash);uint8_t chunk[4096];int result=0;
    while(length){size_t n=length<sizeof(chunk)?length:sizeof(chunk);if(file_read(fd,chunk,n)){result=-1;break;}BrookShaAdd(&hash,chunk,n);length-=n;}
    if(close(fd))result=-1;
    if(!result)BrookShaFinish(&hash,out);
    return result;
}
static int pair_valid(const uint8_t* record) {
    if(!BrookUpdateRecordValid(record))return 0;
    unsigned slot=BrookUpdateGet32(record+16);uint8_t h[32];
    size_t kernel_size=(size_t)BrookUpdateGet64(record+24);
    uint8_t* kernel=malloc(kernel_size);if(!kernel)return 0;
    int fd=open_existing(slot*2);int valid=0;
    if(fd>=0){valid=!file_read(fd,kernel,kernel_size);if(close(fd))valid=0;}
    if(valid)valid=BrookUpdateExecutable(kernel,kernel_size);
    free(kernel);if(!valid)return 0;
    if(hash_file(slot*2,(size_t)BrookUpdateGet64(record+24),h)||!BrookUpdateEqual(h,record+40,32))return 0;
    return !hash_file(slot*2+1,(size_t)BrookUpdateGet64(record+32),h)&&BrookUpdateEqual(h,record+72,32);
}
static int read_records(uint8_t record[2][128]) {
    for(unsigned i=0;i<2;i++) {
        int fd=open_existing(4+i);if(fd<0)return -1;
        int result=file_read(fd,record[i],128);if(close(fd))result=-1;if(result)return -1;
    }
    return 0;
}
static int write_file(unsigned index,const uint8_t* bytes,size_t length) {
    int fd=open_existing(index);if(fd<0)return -1;
    int result=0;mutated=1;
    while(length) {
        size_t chunk=length>4096?4096:length;ssize_t n=write(fd,bytes,chunk);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){result=-1;break;}
        bytes+=n;length-=(size_t)n;
    }
    if(!result&&fsync(fd))result=-1;
    if(close(fd))result=-1;
    return result;
}
static int network_bytes(int fd,uint8_t* p,size_t n) {
    while(n) {
        struct pollfd wait={fd,POLLIN,0};int ready;
        do{ready=poll(&wait,1,15000);}while(ready<0&&errno==EINTR);
        if(ready<=0)return -1;
        ssize_t got=recv(fd,p,n,0);if(got<0&&errno==EINTR)continue;
        if(got<=0)return -1;
        p+=got;n-=(size_t)got;
    }
    return 0;
}
static int reply(int fd,const char* text) {
    size_t left=strlen(text);
    while(left){ssize_t n=send(fd,text,left,MSG_NOSIGNAL);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;text+=n;left-=(size_t)n;}return 0;
}
static int hex32(const char* p) {
    if(strlen(p)!=32)return 0;
    for(unsigned i=0;i<32;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return 0;
    return 1;
}
static int serve(int client,const char* key,const char* boot,const char* transaction) {
    uint8_t header[104];
    if(network_bytes(client,header,sizeof(header))||memcmp(header,"BRUPLOAD",8)||
       !BrookUpdateEqual(header+8,(const uint8_t*)key,32)||
       !BrookUpdateEqual(header+40,(const uint8_t*)boot,32)||
       !BrookUpdateEqual(header+72,(const uint8_t*)transaction,32))return 10;
    uint8_t records[2][128];if(read_records(records))return 11;
    int newest=BrookUpdateNewest(records[0],records[1]);if(newest<0)return 12;
    int active=-1;
    for(unsigned attempt=0;attempt<2;attempt++){
        int candidate=attempt?1-newest:newest;
        if(pair_valid(records[candidate])){active=candidate;break;}
    }
    if(active<0)return 13;
    uint64_t generation=BrookUpdateGet64(records[newest]+8);
    if(generation==UINT64_MAX)return 14;
    generation++;
    unsigned slot=1-BrookUpdateGet32(records[active]+16),selector=1-(unsigned)active;
    char plan[160];snprintf(plan,sizeof(plan),"{\"state\":\"ready\",\"generation\":%" PRIu64 ",\"slot\":%u,\"selector\":%u}\n",generation,slot,selector);
    if(reply(client,plan))return 15;
    uint8_t incoming[128];
    if(network_bytes(client,incoming,128)||!BrookUpdateRecordValid(incoming)||
       BrookUpdateGet64(incoming+8)!=generation||BrookUpdateGet32(incoming+16)!=slot)return 16;
    size_t kb=(size_t)BrookUpdateGet64(incoming+24),ib=(size_t)BrookUpdateGet64(incoming+32);
    uint8_t* kernel=malloc(kb);uint8_t* initrd=malloc(ib);
    if(!kernel||!initrd){free(kernel);free(initrd);return 17;}
    int result=0;uint8_t h[32];
    if(network_bytes(client,kernel,kb)||network_bytes(client,initrd,ib)){result=18;goto done;}
    BrookUpdateHash(kernel,kb,h);if(!BrookUpdateEqual(h,incoming+40,32)){result=19;goto done;}
    BrookUpdateHash(initrd,ib,h);if(!BrookUpdateEqual(h,incoming+72,32)){result=19;goto done;}
    if(!BrookUpdateExecutable(kernel,kb)||initrd[510]!=0x55||initrd[511]!=0xaa){result=20;goto done;}
    if(reply(client,"{\"state\":\"staged\",\"storage_modified\":false}\n")){result=21;goto done;}
    uint8_t activate[8];
    if(network_bytes(client,activate,8)||memcmp(activate,"ACTIVATE",8)){result=22;goto done;}
    // Only the inactive pair is modified, and selection is published last.
    if(write_file(slot*2,kernel,kb)||write_file(slot*2+1,initrd,ib)){result=23;goto done;}
    if(!pair_valid(incoming)){result=24;goto done;}
    if(write_file(4+selector,incoming,128)){result=25;goto done;}
    // Reopen O_RDWR obtains a new FIL and bypasses FAT's whole-file RO cache.
    // Firmware independently rehashes both files before the next kernel load.
    if(read_records(records)||!BrookUpdateEqual(records[selector],incoming,128)){result=26;goto done;}
    printf("HWUPDATE_ACTIVATED generation=%" PRIu64 " slot=%u transaction=%s reboot_required=1\n",generation,slot,transaction);fflush(stdout);
    snprintf(plan,sizeof(plan),"{\"state\":\"activated\",\"generation\":%" PRIu64 ",\"slot\":%u,\"reboot_required\":true}\n",generation,slot);
    // The job receipt is authoritative if the connection disappears here.
    (void)reply(client,plan);
done:
    free(kernel);free(initrd);return result;
}
int main(int argc,char** argv) {
    if(argc!=3||!hex32(argv[1])||!hex32(argv[2])){fputs("usage: HWUPDATE.ELF BOOT_ID32 TRANSACTION_ID32\n",stderr);return 2;}
    const char* root="/media/usb/UPDATES";const char* keypath="/etc/hardware-update-key";
    const char* marker="/etc/hardware-maintenance";const char* lock="/tmp/hardware-update.lock";unsigned port=1235;
#ifdef BROOK_UPDATE_TEST
    root=getenv("BROOK_UPDATE_TEST_ROOT");keypath=getenv("BROOK_UPDATE_TEST_KEY");marker=keypath;lock=getenv("BROOK_UPDATE_TEST_LOCK");port=(unsigned)atoi(getenv("BROOK_UPDATE_TEST_PORT"));
    if(!root||!keypath||!lock||!port)return 2;
#endif
    int check=open(marker,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(check<0)return 3;close(check);
    int fd=open(keypath,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);char key[33]={0};
    if(fd<0)return 3;
    int read_error=file_read(fd,(uint8_t*)key,32);char extra;ssize_t tail=read(fd,&extra,1);int close_error=close(fd);
    if(read_error||tail!=0||close_error||!hex32(key))return 3;
    fd=open(lock,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 4;
    if(close(fd))return 4;
    directory=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);int listener=-1,client=-1,result=5;
    if(directory<0)goto done;
    listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)goto done;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr={.s_addr=htonl(INADDR_ANY)}};
    if(bind(listener,(struct sockaddr*)&address,sizeof(address))||listen(listener,1))goto done;
    puts("HWUPDATE_LISTENING port=1235 maintenance_only=1");fflush(stdout);
    struct pollfd wait={listener,POLLIN,0};if(poll(&wait,1,60000)<=0){result=6;goto done;}
    client=accept(listener,NULL,NULL);if(client<0){result=7;goto done;}
    result=serve(client,key,argv[1],argv[2]);
    if(result){char error[80];snprintf(error,sizeof(error),"{\"state\":\"failed\",\"code\":%d}\n",result);(void)reply(client,error);}
done:
    if(client>=0)close(client);
    if(listener>=0)close(listener);
    if(directory>=0)close(directory);
    // A failed write locks out further updates until a fresh maintenance boot.
    if(!result||!mutated)unlink(lock);
    if(result)fprintf(stderr,"HWUPDATE_FAILED code=%d storage_modified=%u\n",result,mutated);
    return result;
}
