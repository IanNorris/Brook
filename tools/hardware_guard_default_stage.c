/* Promote a separately staged guarded loader, retaining the original and F5.
 * A FAT rename is not atomic replacement: recovery requires the independent
 * firmware F5 entry if power fails between the two renames. */
#define main inventory_stage_unused_main
#include "hardware_efi_inventory_stage.c"
#undef main

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
static int absent(int dir,const char *name) {
    struct stat st;return fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
}
static int create_copy(int dir,const char *name,const uint8_t *data,size_t bytes) {
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0)return -1;
    int err=exact_write(fd,data,bytes)||fsync(fd);if(close(fd))err=1;
    return err?-1:0;
}
int main(int argc,char **argv) {
#if defined(BROOK_GUARD_REPROMOTE_NEXT)
    /* Promote a later staged loader (default BROOKT6.EFI) and keep the current default as
     * BROOK_GUARD_NEXT_BACKUP (default BROOKP5.EFI). Every earlier loader is hash-checked and preserved. */
#ifndef BROOK_GUARD_NEXT_STAGE
#define BROOK_GUARD_NEXT_STAGE "BROOKT6.EFI"
#endif
#ifndef BROOK_GUARD_NEXT_BACKUP
#define BROOK_GUARD_NEXT_BACKUP "BROOKP5.EFI"
#endif
    if(argc!=13||geteuid()!=0){fprintf(stderr,"usage: guard-repromote-next old new f5 kernel initrd selector guard0 guard1 oldbackup g2 v4 v5 hashes\n");return 2;}
    const char *names[]={"BOOTX64.EFI",BROOK_GUARD_NEXT_STAGE,"BROOKF5.EFI","KERNEL.ELF","INITRD.IMG","SELECT.BIN","GUARD0.BIN","GUARD1.BIN",
                         "BROOKOLD.EFI","BROOKG2.EFI","BROOKV4.EFI","BROOKV5.EFI"};
    const int itemCount=12;const char *backupName=BROOK_GUARD_NEXT_BACKUP;
#elif defined(BROOK_GUARD_REPROMOTE)
    /* Preserve both the first original backup and its separately staged loader. */
    if(argc!=11||geteuid()!=0){fprintf(stderr,"usage: guard-repromote old new f5 kernel initrd selector guard0 guard1 oldbackup oldstage hashes\n");return 2;}
    const char *names[]={"BOOTX64.EFI","BROOKV5.EFI","BROOKF5.EFI","KERNEL.ELF","INITRD.IMG","SELECT.BIN","GUARD0.BIN","GUARD1.BIN","BROOKOLD.EFI","BROOKG2.EFI"};
    const int itemCount=10;const char *backupName="BROOKV4.EFI";
#else
    /* old loader, new loader, F5, kernel, initrd, selector, guard0, guard1 */
    if(argc!=9||geteuid()!=0){fprintf(stderr,"usage: guard-default old new f5 kernel initrd selector guard0 guard1 hashes\n");return 2;}
    const char *names[]={"BOOTX64.EFI","BROOKG2.EFI","BROOKF5.EFI","KERNEL.ELF","INITRD.IMG","SELECT.BIN","GUARD0.BIN","GUARD1.BIN"};
    const int itemCount=8;const char *backupName="BROOKOLD.EFI";
#endif
    int root=-1,efi=-1,boot=-1,rec=-1,legacy=-1,lock=-1,result=1,mutated=0;
    uint8_t *data[12]={0};size_t sizes[12]={0};struct stat rst,st;
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||fstat(root,&rst))goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(lock<0)goto done;
    efi=openat(root,"EFI",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(efi<0||ioctl(efi,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0))goto done;
    boot=openat(efi,"BOOT",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    rec=openat(root,"RECOV2",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    legacy=openat(root,"RECOVERY",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(boot<0||rec<0||legacy<0||ioctl(boot,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||
       ioctl(rec,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||ioctl(legacy,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||
       fstat(boot,&st)||st.st_dev!=rst.st_dev||!absent(boot,backupName)||!absent(boot,"BROOKNEW.EFI"))goto done;
    for(int i=0;i<itemCount;++i)if(read_checked(i<3?boot:i<6?rec:i<8?legacy:boot,names[i],argv[i+1],&data[i],&sizes[i]))goto done;
    if(!valid_efi(data[0],sizes[0])||!valid_efi(data[1],sizes[1])||!valid_efi(data[2],sizes[2])||
       sizes[6]!=128||sizes[7]!=128||memcmp(data[6],data[7],128))goto done;
    /* A complete durable new copy exists before removing the default name. */
    mutated=1;
    if(create_copy(boot,"BROOKNEW.EFI",data[1],sizes[1])||fsync(boot))goto done;
    {uint8_t *check=NULL;size_t n=0;int err=read_checked(boot,"BROOKNEW.EFI",argv[2],&check,&n);free(check);if(err||n!=sizes[1])goto done;}
    if(renameat(boot,"BOOTX64.EFI",boot,backupName)||fsync(boot)||fsync(efi)||fsync(root))goto done;
    if(renameat(boot,"BROOKNEW.EFI",boot,"BOOTX64.EFI")||fsync(boot)||fsync(efi)||fsync(root))goto done;
    {uint8_t *check=NULL;size_t n=0;int err=read_checked(boot,"BOOTX64.EFI",argv[2],&check,&n);free(check);if(err||n!=sizes[1])goto done;}
    {uint8_t *check=NULL;size_t n=0;int err=read_checked(boot,backupName,argv[1],&check,&n);free(check);if(err||n!=sizes[0])goto done;}
    result=0;
done:
    for(int i=0;i<12;++i)free(data[i]);
    if(legacy>=0)close(legacy);
    if(rec>=0)close(rec);
    if(boot>=0)close(boot);
    if(efi>=0)close(efi);
    if(root>=0)close(root);
    if(lock>=0){close(lock);if((!result||!mutated)&&unlink("/tmp/hardware-update.lock"))result=1;}
    if(result)fprintf(stderr,"GUARD_DEFAULT_FAILED mutated=%d; inspect before retry\n",mutated);
    else puts("GUARD_DEFAULT_OK firmware selection unverified; original and F5 retained");
    return result;
}
