/* First-time guard provisioning on the boot-admitted SSD system volume.
 * Never replace an existing guard, loader, recovery payload or selector. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"
#include "../src/shared/update/recovery_guard.h"
#include "../src/shared/update/guard_slot_health.h"

#include "hardware_provision_identity.h"

constexpr unsigned SlotHealthBytes=brook::GuardSlotCount*brook::GuardSlotSectorBytes;

static int read_record(int directory,const char* name,uint8_t* record,size_t size,dev_t device) {
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat st;
    size_t done=0;
    int error=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_dev!=device||st.st_size!=off_t(size);
    while(!error && done<size) {
        const ssize_t n=read(fd,record+done,size-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=1;break;}
        done+=size_t(n);
    }
    if(close(fd))error=1;
    return error?-1:0;
}

static int create_record(int directory,const char* name,const uint8_t* record,size_t size,dev_t device,
                         bool* created=nullptr) {
    if(created)*created=false;
    uint8_t check[SlotHealthBytes];
    if(size>sizeof(check))return -1;
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0)return -1;
    if(created)*created=true;
    size_t done=0;
    int error=0;
    while(done<size) {
        const ssize_t n=write(fd,record+done,size-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=1;break;}
        done+=size_t(n);
    }
    if(!error && fsync(fd))error=1;
    if(close(fd))error=1;
    if(error || read_record(directory,name,check,size,device))return -1;
    return memcmp(record,check,size)?-1:0;
}

static int slot_capacity(int directory) {
    struct statfs space{};
    if(fstatfs(directory,&space))return -1;
    if(space.f_bsize<=0){errno=EIO;return -1;}
    const uint64_t cluster=uint64_t(space.f_bsize);
    // Leave room for both directory entries even if FAT needs to grow the
    // RECOVERY directory while staging and publishing.
    const uint64_t needed=(SlotHealthBytes+cluster-1)/cluster+2;
    if(uint64_t(space.f_bavail)<needed){errno=ENOSPC;return -1;}
    return 0;
}

static int verify_clean_guards(int root,int directory,dev_t device,const char (&identity)[97]) {
    uint8_t digest[32];
    BrookUpdateHash(reinterpret_cast<const uint8_t*>(identity),96,digest);
    const int result=ioctl(root,BROOK_IOCTL_CHECK_SYSTEM_ESP_GUARD_IDENTITY,digest);
    if(result==0 || result==BROOK_ESP_GUARD_PINNED_SLOT_HEALTH)return result;
    if(result!=-1 || errno!=ENOTTY)return -1;

    // Old or non-guarded kernels retain ordinary access to the guard files.
    // Any other ioctl failure means that admission could not be established.
    uint8_t guards[2][BROOK_RECOVERY_GUARD_SIZE];
    if(read_record(directory,"GUARD0.BIN",guards[0],sizeof(guards[0]),device) ||
       read_record(directory,"GUARD1.BIN",guards[1],sizeof(guards[1]),device))return -1;
    if(BrookRecoveryGuardAllowsNormal(guards[0],sizeof(guards[0]),guards[1],sizeof(guards[1]),identity))
        return 0;
    errno=ESTALE;
    return -1;
}

static int provision_slot_health(int root,int directory,dev_t device,const char (&identity)[97],int& mutated) {
    const int admitted=verify_clean_guards(root,directory,device,identity);
    if(admitted<0)return -1;
    if(admitted==BROOK_ESP_GUARD_PINNED_SLOT_HEALTH)return 0;
    struct SlotFile {
        uint8_t bytes[SlotHealthBytes]{};
        bool read(unsigned index,uint8_t* out) const {
            memcpy(out,bytes+index*brook::GuardSlotSectorBytes,brook::GuardSlotSectorBytes);
            return true;
        }
    } file;
    constexpr char name[]="BOOTSTAT.BIN",temp[]="BOOTSTAT.TMP";
    struct stat existing{};
    const bool present=fstatat(directory,name,&existing,AT_SYMLINK_NOFOLLOW)==0;
    if(present) {
        if(!S_ISREG(existing.st_mode)||existing.st_dev!=device)return -1;
        if(existing.st_size) {
            if(read_record(directory,name,file.bytes,sizeof(file.bytes),device))return -1;
            uint8_t digest[32];
            BrookUpdateHash(reinterpret_cast<const uint8_t*>(identity),96,digest);
            brook::GuardSlotJournal journal;
            return brook::GuardSlotRead(file,digest,journal)?0:-1;
        }
        // A zero-byte file from older, interrupted provisioning is equivalent
        // to the absent journal. Leave it intact until the replacement is ready.
    } else if(errno!=ENOENT)return -1;

    struct stat stage{};
    if(fstatat(directory,temp,&stage,AT_SYMLINK_NOFOLLOW)==0) {
        uint8_t previous[SlotHealthBytes]{};
        if(!S_ISREG(stage.st_mode)||stage.st_dev!=device||stage.st_size<0||
           uint64_t(stage.st_size)>sizeof(previous)||
           read_record(directory,temp,previous,size_t(stage.st_size),device)||
           !BrookUpdateEqual(previous,file.bytes,sizeof(previous)))return -1;
        if(unlinkat(directory,temp,0))return -1;
        if(fsync(directory)){mutated=1;return -1;}
    } else if(errno!=ENOENT)return -1;

    if(slot_capacity(directory))return -1;
    auto abandon=[&]() {
        const int failure=errno;
        if((unlinkat(directory,temp,0) && errno!=ENOENT) || fsync(directory))mutated=1;
        errno=failure;
        return -1;
    };
    bool created=false;
    if(create_record(directory,temp,file.bytes,sizeof(file.bytes),device,&created))
        return created?abandon():-1;

    if(present) {
        struct stat current{};
        if(fstatat(directory,name,&current,AT_SYMLINK_NOFOLLOW) || !S_ISREG(current.st_mode) ||
           current.st_dev!=device || current.st_ino!=existing.st_ino || current.st_size ||
           unlinkat(directory,name,0))return abandon();
    }
    // Use the kernel's same-directory, no-replace transaction. A separately
    // published final journal is never replaced, even by a racing root tool.
    if(syscall(SYS_renameat2,directory,temp,directory,name,1 /* RENAME_NOREPLACE */))
        return abandon();
    mutated=1;
    uint8_t readback[SlotHealthBytes]{};
    return fsync(directory) || read_record(directory,name,readback,sizeof(readback),device) ||
        !BrookUpdateEqual(file.bytes,readback,sizeof(readback)) ? -1 : 0;
}

int main(int argc,char **argv) {
    const bool slotOnly=argc==3&&!strcmp(argv[2],"--slot-health-only");
    if((argc!=2&&!slotOnly)||geteuid()!=0||strlen(argv[1])!=96) {
        fprintf(stderr,"usage: guard-provision expected-identity [--slot-health-only]\n");return 2;
    }
    char identity[97];uint8_t record[128]={0};
    if(load_identity(identity)||strcmp(identity,argv[1])) {fprintf(stderr,"GUARD_PROVISION_IDENTITY_MISMATCH\n");return 1;}
    memcpy(record,"BRKRCV01",8);BrookUpdatePut64(record+8,1);BrookUpdatePut32(record+16,BROOK_RECOVERY_GUARD_CLEAN);
    for(unsigned i=0;i<48;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {char c=identity[i*2+j];value=value*16+(unsigned)(c<='9'?c-'0':c-'a'+10);}
        record[24+i]=(uint8_t)value;
    }
    BrookUpdatePut32(record+124,BrookUpdateCrc(record,124));
    if(!BrookRecoveryGuardAllowsNormal(record,128,record,128,identity))return 1;
    int root=-1,dir=-1,lock=-1,result=1,mutated=0;struct stat rst,dst,check;
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||fstat(root,&rst))goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0)goto done;
    dir=openat(root,"RECOVERY",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0||ioctl(dir,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||fstat(dir,&dst)||dst.st_dev!=rst.st_dev)goto done;
    if(slotOnly) {
        if(provision_slot_health(root,dir,rst.st_dev,identity,mutated) ||
           (mutated && (fsync(dir)||fsync(root))))goto done;
    } else {
        if(fstatat(dir,"GUARD0.BIN",&check,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)goto done;
        if(fstatat(dir,"GUARD1.BIN",&check,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)goto done;
        mutated=1;
        if(create_record(dir,"GUARD0.BIN",record,sizeof(record),rst.st_dev)||fsync(dir)||fsync(root))goto done;
        if(create_record(dir,"GUARD1.BIN",record,sizeof(record),rst.st_dev)||fsync(dir)||fsync(root))goto done;
    }
    {
        int fresh=openat(root,"RECOVERY",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fresh<0)goto done;
        int error=ioctl(fresh,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||fstat(fresh,&check)||
                  check.st_dev!=dst.st_dev||check.st_ino!=dst.st_ino;
        if(close(fresh))error=1;
        if(error)goto done;
    }
    result=0;
done:
    if(dir>=0)close(dir);
    if(root>=0)close(root);
    if(lock>=0) {close(lock);if((!result||!mutated)&&unlink("/tmp/hardware-update.lock"))result=1;}
    if(result)fprintf(stderr,"GUARD_PROVISION_FAILED mutated=%d; inspect before retry\n",mutated);
    else puts(slotOnly?"GUARD_SLOT_HEALTH_PROVISION_OK":"GUARD_PROVISION_OK loader unchanged");
    return result;
}
