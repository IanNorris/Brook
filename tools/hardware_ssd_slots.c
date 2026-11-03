/* Initialize A/B boot slots on an already admitted SSD system volume.
 * Root kernel/initrd remain untouched. Run only from the trusted maintenance
 * environment; the original boot configuration is modified last. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "../src/shared/update/payload.h"
#include "../src/shared/update/executable.h"
static unsigned mutated;

static int checkpoint(const char *stage) {
#ifdef BROOK_SSD_SLOT_TEST
    const char *hold=getenv("BROOK_TEST_HOLD");
    if(hold && !strcmp(hold,stage)) {
        printf("SSD_SLOT_TEST_HOLD %s\n",stage);fflush(stdout);
        for(;;)sleep(1);
    }
    const char *stop=getenv("BROOK_TEST_STOP");return stop && !strcmp(stop,stage);
#else
    (void)stage;return 0;
#endif
}

static int read_exact(int fd, void *data, size_t length) {
    uint8_t *p=data;
    while(length) { ssize_t n=read(fd,p,length); if(n<0 && errno==EINTR)continue; if(n<=0)return -1; p+=n; length-=(size_t)n; }
    return 0;
}
static int write_exact(int fd, const void *data, size_t length) {
    const uint8_t *p=data;
    while(length) { ssize_t n=write(fd,p,length); if(n<0 && errno==EINTR)continue; if(n<=0)return -1; p+=n; length-=(size_t)n; }
    return 0;
}
static int load_file(int dir, const char *name, size_t minimum, size_t maximum, uint8_t **data, size_t *length) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<(off_t)minimum||(uint64_t)st.st_size>maximum){close(fd);return -1;}
    uint8_t *p=malloc((size_t)st.st_size);if(!p){close(fd);return -1;}
    int error=read_exact(fd,p,(size_t)st.st_size);if(close(fd))error=-1;
    if(error){free(p);return -1;}*data=p;*length=(size_t)st.st_size;return 0;
}
static int verify_file(int dir,const char *name,const uint8_t *data,size_t length,size_t capacity) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); struct stat st; uint8_t buf[65536]; size_t off=0;
    if(fd<0)return -1;
    int error=fstat(fd,&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size!=capacity;
    while(!error && off<length){size_t n=length-off>sizeof(buf)?sizeof(buf):length-off;
        if(read_exact(fd,buf,n)||memcmp(buf,data+off,n))error=1;
        off+=n;}
    if(close(fd))error=1;
    return error?-1:0;
}
static int create_file(int dir,const char *name,const uint8_t *data,size_t length,size_t capacity) {
    if(length>capacity)return -1;
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0)return -1;
    int error=write_exact(fd,data,length);uint8_t zeros[65536]={0};size_t left=capacity-length;
    while(!error && left){size_t n=left>sizeof(zeros)?sizeof(zeros):left;error=write_exact(fd,zeros,n);left-=n;}
    if(!error && fsync(fd))error=-1;
    if(close(fd))error=-1;
    return error?-1:verify_file(dir,name,data,length,capacity);
}
static void make_record(uint8_t *r,uint64_t generation,uint32_t slot,
                        const uint8_t *kernel,size_t kb,const uint8_t *initrd,size_t ib) {
    memset(r,0,128);memcpy(r,"BROOKUP1",8);BrookUpdatePut64(r+8,generation);BrookUpdatePut32(r+16,slot);
    BrookUpdatePut64(r+24,kb);BrookUpdatePut64(r+32,ib);BrookUpdateHash(kernel,kb,r+40);BrookUpdateHash(initrd,ib,r+72);
    BrookUpdatePut32(r+124,BrookUpdateCrc(r,124));
}
/* The bootloader applies configuration entries in order. Accept one exact
 * disabled setting, since the final appended setting enables the slots. Any
 * other occurrence is ambiguous and must be inspected before mutation. */
static int config_allows_activation(const uint8_t *config,size_t length) {
    static const char key[]="UPDATE_SLOTS", disabled[]="UPDATE_SLOTS=0";
    size_t seen=0;
    for(size_t at=0;at+sizeof(key)-1<=length;++at) {
        if(memcmp(config+at,key,sizeof(key)-1))continue;
        if(++seen>1 || (at && config[at-1]!='\n') ||
           at+sizeof(disabled)-1>length || memcmp(config+at,disabled,sizeof(disabled)-1))return 0;
        size_t end=at+sizeof(disabled)-1;
        if(end<length && config[end]=='\r')++end;
        if(end<length && config[end]!='\n')return 0;
    }
    return 1;
}
static int setup(int root,const uint8_t expectedEfi[32]) {
    int result=1,updates=-1,a=-1,b=-1,configfd=-1;uint8_t *kernel=NULL,*initrd=NULL,*config=NULL,*efi=NULL;
    size_t kb=0,ib=0,cb=0,eb=0;uint8_t records[2][128],efiHash[32];struct statvfs freeSpace;
    static const char activation[]="\nUPDATE_SLOTS=1\n";
    int activationAttempted=0;
    if(fstatvfs(root,&freeSpace)||!freeSpace.f_frsize||freeSpace.f_bavail>UINT64_MAX/freeSpace.f_frsize ||
       (uint64_t)freeSpace.f_bavail*freeSpace.f_frsize<2ULL*(BROOK_UPDATE_KERNEL_MAX+BROOK_UPDATE_INITRD_MAX)+1048576)goto done;
    if(load_file(root,"EFI/BOOT/BOOTX64.EFI",64,8*1024*1024,&efi,&eb))goto done;
    BrookUpdateHash(efi,eb,efiHash);if(!BrookUpdateEqual(efiHash,expectedEfi,32))goto done;
    if(load_file(root,"KERNEL/BROOK.ELF",64,BROOK_UPDATE_KERNEL_MAX,&kernel,&kb)||!BrookUpdateExecutable(kernel,kb)||
       load_file(root,"INITRD.IMG",512,BROOK_UPDATE_INITRD_MAX,&initrd,&ib)||
       load_file(root,"BROOK.CFG",1,4096,&config,&cb))goto done;
    /* Reject existing or ambiguous configuration rather than changing an
     * already configured installation. The buffer is NUL-terminated here. */
    for(size_t i=0;i<cb;++i)if(!config[i])goto done;
    if(cb>4096-(sizeof(activation)-1) || !config_allows_activation(config,cb))goto done;
    make_record(records[0],1,0,kernel,kb,initrd,ib);make_record(records[1],2,1,kernel,kb,initrd,ib);
    if(!BrookUpdateRecordValid(records[0])||!BrookUpdateRecordValid(records[1]))goto done;
    mutated=1;
    if(mkdirat(root,"UPDATES",0755))goto done;
    if(checkpoint("directory"))goto done;
    updates=openat(root,"UPDATES",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(updates<0)goto done;
    if(mkdirat(updates,"A",0755)||mkdirat(updates,"B",0755))goto done;
    a=openat(updates,"A",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    b=openat(updates,"B",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(a<0||b<0)goto done;
    if(create_file(a,"KERNEL.ELF",kernel,kb,BROOK_UPDATE_KERNEL_MAX))goto done;
    if(checkpoint("slot-a-kernel"))goto done;
    if(create_file(a,"INITRD.IMG",initrd,ib,BROOK_UPDATE_INITRD_MAX)||fsync(a))goto done;
    if(checkpoint("slot-a"))goto done;
    if(create_file(b,"KERNEL.ELF",kernel,kb,BROOK_UPDATE_KERNEL_MAX))goto done;
    if(checkpoint("slot-b-kernel"))goto done;
    if(create_file(b,"INITRD.IMG",initrd,ib,BROOK_UPDATE_INITRD_MAX)||fsync(b))goto done;
    if(checkpoint("slot-b"))goto done;
    if(create_file(updates,"SELECT0.BIN",records[0],128,128))goto done;
    if(checkpoint("selector-a"))goto done;
    if(create_file(updates,"SELECT1.BIN",records[1],128,128)||fsync(updates)||fsync(root))goto done;
    if(checkpoint("selectors"))goto done;
    /* Verify config has not changed since inspection. Append activation last;
     * the root recovery pair is never opened for writing. */
    uint8_t *check=NULL;size_t checkLen=0;
    if(load_file(root,"BROOK.CFG",1,4096,&check,&checkLen)||checkLen!=cb||memcmp(check,config,cb)){free(check);goto done;}free(check);
    if(verify_file(root,"KERNEL/BROOK.ELF",kernel,kb,kb)||verify_file(root,"INITRD.IMG",initrd,ib,ib)||
       verify_file(root,"EFI/BOOT/BOOTX64.EFI",efi,eb,eb))goto done;
    configfd=openat(root,"BROOK.CFG",O_WRONLY|O_APPEND|O_NOFOLLOW|O_CLOEXEC);if(configfd<0)goto done;
    activationAttempted=1;
    if(write_exact(configfd,activation,sizeof(activation)-1))goto done;
    if(checkpoint("activation-written"))goto done;
    if(fsync(configfd))goto done;
    if(close(configfd)){configfd=-1;goto done;}configfd=-1;
    check=NULL;checkLen=0;
    if(load_file(root,"BROOK.CFG",1,4096,&check,&checkLen)||checkLen!=cb+sizeof(activation)-1||
       memcmp(check,config,cb)||memcmp(check+cb,activation,sizeof(activation)-1)){free(check);goto done;}
    free(check);if(fsync(root))goto done;result=0;
done:
    if(result)fprintf(stderr,"SSD_SLOT_SETUP_ERROR activation_attempted=%d; inspect before retry\n",activationAttempted);
    if(configfd>=0)close(configfd);
    if(a>=0)close(a);
    if(b>=0)close(b);
    if(updates>=0)close(updates);
    free(kernel);free(initrd);free(config);free(efi);return result;
}
static int hash_arg(const char *text,uint8_t hash[32]) {
    if(strlen(text)!=64)return -1;
    for(unsigned i=0;i<32;++i){unsigned v=0;for(unsigned j=0;j<2;++j){char c=text[i*2+j];unsigned n;
        if(c>='0'&&c<='9')n=(unsigned)(c-'0');else if(c>='a'&&c<='f')n=(unsigned)(c-'a'+10);else return -1;
        v=v*16+n;}hash[i]=(uint8_t)v;}
    return 0;
}
int main(int argc,char **argv) {
    uint8_t expectedEfi[32];if(argc!=2||geteuid()!=0||hash_arg(argv[1],expectedEfi))return 2;
    int root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st,parent;struct statfs fs;
    if(root<0||fstat(root,&st)||stat("/media",&parent)||fstatfs(root,&fs)||
       (uint64_t)fs.f_type!=0x4d44 || st.st_dev==parent.st_dev){if(root>=0)close(root);return 3;}
    const char* markers[]={"/etc/hardware-maintenance","/etc/ahci-boot-volume","/etc/ahci-write-target"};
    for(unsigned i=0;i<sizeof(markers)/sizeof(markers[0]);++i){
        int marker=open(markers[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(marker<0){close(root);return 4;}close(marker);
    }
    int lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0){close(root);return 5;}
    int result=setup(root,expectedEfi);close(root);close(lock);
    // As with the updater, uncertain partial mutations retain the lock until
    // reboot and require explicit inspection rather than an automatic retry.
    if((!result || !mutated) && unlink("/tmp/hardware-update.lock"))result=1;
    if(result)fprintf(stderr,"SSD_SLOT_SETUP_FAILED; inspect existing files before retry\n");
    else puts("SSD_SLOT_SETUP_OK generation=2");
    return result;
}
