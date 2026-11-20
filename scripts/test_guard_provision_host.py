#!/usr/bin/env python3
"""Check guard provisioning primitives without touching a guest disk."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <errno.h>
#include <stdarg.h>
#include <initializer_list>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <unistd.h>
static unsigned fail_sync=0;
static bool low_space=false;
static int guard_ioctl_result=-1, guard_ioctl_error=ENOTTY;
static int expected_root=-1;
static uint8_t expected_digest[32]{};
static int test_ioctl(int fd,unsigned long request,...);
static int test_sync(int fd) {
    if(fail_sync && --fail_sync==0){errno=EIO;return -1;}
    return fsync(fd);
}
static int test_space(int fd, struct statfs* space) {
    if(low_space) {
        *space={};space->f_bsize=512;space->f_bavail=2;return 0;
    }
    return fstatfs(fd,space);
}
#define ioctl test_ioctl
#define fsync test_sync
#define fstatfs test_space
#define main guard_provision_main
#include "hardware_guard_provision.cpp"
#undef main
#undef ioctl
#undef fsync
#undef fstatfs
static int test_ioctl(int fd,unsigned long request,...) {
    if(fd!=expected_root || request!=BROOK_IOCTL_CHECK_SYSTEM_ESP_GUARD_IDENTITY){
        errno=EINVAL;
        return -1;
    }
    va_list args;
    va_start(args,request);
    const auto* digest=va_arg(args,uint8_t*);
    va_end(args);
    if(!BrookUpdateEqual(digest,expected_digest,sizeof(expected_digest))){
        errno=ESTALE;
        return -1;
    }
    errno=guard_ioctl_error;
    return guard_ioctl_result;
}
int main(int argc,char **argv) {
    if(argc!=2)return 1;
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY);struct stat st;
    if(dir<0||fstat(dir,&st))return 2;
    uint8_t record[128]={};memcpy(record,"BRKRCV01",8);
    BrookUpdatePut64(record+8,1);BrookUpdatePut32(record+16,BROOK_RECOVERY_GUARD_CLEAN);
    for(unsigned i=24;i<72;++i)record[i]=(uint8_t)i;
    BrookUpdatePut32(record+124,BrookUpdateCrc(record,124));
    if(create_record(dir,"GUARD0.BIN",record,sizeof(record),st.st_dev))return 3;
    record[24]^=1;
    if(!create_record(dir,"GUARD0.BIN",record,sizeof(record),st.st_dev))return 4;
    int fd=openat(dir,"GUARD0.BIN",O_RDONLY);uint8_t saved[128];
    if(fd<0||read(fd,saved,sizeof(saved))!=128||saved[24]!=(uint8_t)24)return 5;
    close(fd);
    char identity[97]{};
    const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<48;++i) {
        identity[i*2]=hex[saved[24+i]>>4];
        identity[i*2+1]=hex[saved[24+i]&15];
    }
    // Exercise the separate root descriptor and exact expected digest.
    const int root=dup(dir);
    if(root<0)return 27;
    expected_root=root;
    BrookUpdateHash(reinterpret_cast<const uint8_t*>(identity),96,expected_digest);
    int mutated=0;
    if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated)return 7;
    guard_ioctl_result=0;
    if(verify_clean_guards(root,dir,st.st_dev,identity))return 28; // Leased records need no reopen.
    char mismatch[97];memcpy(mismatch,identity,sizeof(mismatch));mismatch[0]='f';
    if(!verify_clean_guards(root,dir,st.st_dev,mismatch)||errno!=ESTALE)return 29;
    guard_ioctl_result=-1;
    if(create_record(dir,"GUARD1.BIN",saved,sizeof(saved),st.st_dev))return 8;
    for(const int denial : {EPERM, ENODEV, EAGAIN, EIO, ESTALE, EBUSY}) {
        guard_ioctl_error=denial;
        if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated||errno!=denial)return 30;
    }
    guard_ioctl_error=ENOTTY;
    guard_ioctl_result=BROOK_ESP_GUARD_PINNED_SLOT_HEALTH;
    if(provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated)return 31;
    struct stat pinnedProbe{};
    if(fstatat(dir,"BOOTSTAT.BIN",&pinnedProbe,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)return 32;
    guard_ioctl_result=2; // Unexpected successful return must not fall back.
    if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated)return 33;
    guard_ioctl_result=-1;
    low_space=true;
    if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated||errno!=ENOSPC)return 16;
    low_space=false;
    struct stat entry{};
    if(fstatat(dir,"BOOTSTAT.BIN",&entry,AT_SYMLINK_NOFOLLOW)==0||
       fstatat(dir,"BOOTSTAT.TMP",&entry,AT_SYMLINK_NOFOLLOW)==0)return 17;
    fail_sync=1; // Fail the staged file fsync, then verify cleanup and retry.
    if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated)return 18;
    if(fstatat(dir,"BOOTSTAT.BIN",&entry,AT_SYMLINK_NOFOLLOW)==0||
       fstatat(dir,"BOOTSTAT.TMP",&entry,AT_SYMLINK_NOFOLLOW)==0)return 19;
    uint8_t blank[SlotHealthBytes]{};
    if(create_record(dir,"BOOTSTAT.TMP",blank,17,st.st_dev)||
       create_record(dir,"BOOTSTAT.BIN",blank,0,st.st_dev))return 20;
    if(provision_slot_health(root,dir,st.st_dev,identity,mutated)||!mutated)return 9;
    if(fstatat(dir,"BOOTSTAT.TMP",&entry,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)return 21;
    uint8_t slot[SlotHealthBytes];
    if(read_record(dir,"BOOTSTAT.BIN",slot,sizeof(slot),st.st_dev))return 10;
    for(auto byte:slot)if(byte)return 11;
    mutated=0;
    if(provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated)return 12;

    fd=openat(dir,"BOOTSTAT.BIN",O_WRONLY);
    const uint8_t bad=1;
    if(fd<0||pwrite(fd,&bad,1,0)!=1||close(fd))return 13;
    if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated)return 14;
    if(read_record(dir,"BOOTSTAT.BIN",slot,sizeof(slot),st.st_dev)||slot[0]!=bad)return 15;
    if(unlinkat(dir,"BOOTSTAT.BIN",0)||create_record(dir,"BOOTSTAT.BIN",&bad,1,st.st_dev))return 22;
    if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated||
       read_record(dir,"BOOTSTAT.BIN",slot,1,st.st_dev)||slot[0]!=bad)return 23;
    if(unlinkat(dir,"BOOTSTAT.BIN",0))return 24;
    fail_sync=2; // Directory flush must be acknowledged before success.
    if(!provision_slot_health(root,dir,st.st_dev,identity,mutated)||!mutated)return 25;
    mutated=0;
    if(provision_slot_health(root,dir,st.st_dev,identity,mutated)||mutated)return 26;
    close(root);close(dir);
    if(!hex32("0123456789abcdef0123456789abcdef") || hex32("bad") ||
       !range("2048,4096") || range("2048,") || range("1,2,3"))return 6;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='brook-guard-provision-') as temporary:
    path=Path(temporary);(path/'test.cpp').write_text(HARNESS);(path/'target').mkdir()
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'tools'),
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test'),str(path/'target')],check=True)
print('guard provisioning primitives: PASS')
