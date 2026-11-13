/* Promote an already staged recovery pair, retaining both the old alternate
 * and the independent legacy recovery pair. A reset between renames falls
 * back to RECOVERY; inspect the retained lock after any uncertain mutation. */
#define main inventory_stage_unused_main
#include "hardware_efi_inventory_stage.c"
#undef main
#include "../src/shared/update/executable.h"

static int absent(int dir,const char *name) {
    struct stat st;return fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
}
static int read_checked(int dir,const char *name,const char *expected,uint8_t **out,size_t *bytes) {
    uint8_t wanted[32],actual[32];struct stat st;
    if(decode_hash(expected,wanted))return -1;
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>32*1024*1024){close(fd);return -1;}
    uint8_t *data=malloc((size_t)st.st_size);
    if(!data){close(fd);return -1;}
    int err=exact_read(fd,data,(size_t)st.st_size);if(close(fd))err=1;
    if(!err){BrookUpdateHash(data,(size_t)st.st_size,actual);err=!BrookUpdateEqual(actual,wanted,32);}
    if(err){free(data);return -1;}
    *out=data;*bytes=(size_t)st.st_size;return 0;
}

static int checked_pair(int dir,char **hashes) {
    const char *names[]={"KERNEL.ELF","INITRD.IMG","SELECT.BIN"};
    uint8_t *data[3]={0},hash[32];size_t size[3]={0};int error=0;
    for(int i=0;i<3 && !error;++i)error=read_checked(dir,names[i],hashes[i],&data[i],&size[i]);
    if(!error && (size[2]!=128 || !BrookUpdateRecordValid(data[2]) ||
       BrookUpdateGet32(data[2]+16)!=0 || BrookUpdateGet64(data[2]+24)!=size[0] ||
       BrookUpdateGet64(data[2]+32)!=size[1] || !BrookUpdateExecutable(data[0],size[0])))error=1;
    if(!error){BrookUpdateHash(data[0],size[0],hash);if(!BrookUpdateEqual(hash,data[2]+40,32))error=1;}
    if(!error){BrookUpdateHash(data[1],size[1],hash);if(!BrookUpdateEqual(hash,data[2]+72,32))error=1;}
    for(int i=0;i<3;++i)free(data[i]);
    return error?-1:0;
}
int main(int argc,char **argv) {
    if(argc!=10||geteuid()!=0){fprintf(stderr,"usage: recovery-promote old-pair new-pair legacy-pair hashes\n");return 2;}
    const char *dirs[]={"RECOV2","RECOVNEW","RECOVERY"};
    int root=-1,lock=-1,fd[3]={-1,-1,-1},result=1,mutated=0;
    struct stat rootst,original[3],current;
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0 || ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0) || fstat(root,&rootst))goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0 || !absent(root,"RECOVBAK"))goto done;
    for(int i=0;i<3;++i) {
        fd[i]=openat(root,dirs[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd[i]<0 || ioctl(fd[i],BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0) ||
           fstat(fd[i],&original[i]) || original[i].st_dev!=rootst.st_dev ||
           checked_pair(fd[i],argv+1+3*i))goto done;
    }
    // Brook refuses to rename a directory while a descriptor pins it.
    for(int i=0;i<3;++i) {if(close(fd[i])){fd[i]=-1;goto done;}fd[i]=-1;}
    // A failed rename or flush may still have changed persistent state.
    mutated=1;
    if(renameat(root,"RECOV2",root,"RECOVBAK") || fsync(root))goto done;
    if(renameat(root,"RECOVNEW",root,"RECOV2") || fsync(root))goto done;
    for(int i=0;i<2;++i) {
        int verify=openat(root,i?"RECOVBAK":"RECOV2",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(verify<0)goto done;
        int err=ioctl(verify,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0) || fstat(verify,&current) ||
            current.st_dev!=original[i?0:1].st_dev || current.st_ino!=original[i?0:1].st_ino ||
            checked_pair(verify,argv+1+(i?0:3));
        if(close(verify))err=1;
        if(err)goto done;
    }
    result=0;
done:
    for(int i=0;i<3;++i)if(fd[i]>=0)close(fd[i]);
    if(root>=0)close(root);
    if(lock>=0){close(lock);if((!result||!mutated) && unlink("/tmp/hardware-update.lock"))result=1;}
    if(result)fprintf(stderr,"RECOVERY_PROMOTE_FAILED mutated=%d; inspect before retry\n",mutated);
    else puts("RECOVERY_PROMOTE_OK alternate backup and legacy recovery retained");
    return result;
}
