/* Read-only inventory for an identity-admitted maintenance boot. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"
#include "../src/shared/update/recovery_guard.h"

static int inventory(int root, const char *path, uint8_t *record) {
    int fd=openat(root,path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); struct stat st;
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>32*1024*1024) return -1;
    BrookSha256 hash; BrookShaInit(&hash); uint8_t bytes[4096],digest[32]; size_t off=0;
    for (;;) {
        ssize_t n=read(fd,bytes,sizeof(bytes));
        if(n<0) { close(fd); return -1; }
        if(!n)break;
        if(record && off+(size_t)n<=128)memcpy(record+off,bytes,(size_t)n);
        off+=(size_t)n;BrookShaAdd(&hash,bytes,(size_t)n);
    }
    if(close(fd) || off!=(size_t)st.st_size || (record && off!=128))return -1;
    BrookShaFinish(&hash,digest);printf("%s ",path);
    for(unsigned i=0;i<32;++i)printf("%02x",digest[i]);
    printf(" %zu\n",off);return 0;
}
int main(int argc,char **argv) {
    if(argc!=2||geteuid()!=0||strlen(argv[1])!=96)return 2;
    int root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0))return 1;
    const char *paths[]={"EFI/BOOT/BOOTX64.EFI","EFI/BOOT/BROOKF5.EFI",
        "EFI/BOOT/BROOKOLD.EFI","EFI/BOOT/BROOKG2.EFI","EFI/BOOT/BRKMODE.ELF",
        "RECOV2/KERNEL.ELF","RECOV2/INITRD.IMG","RECOV2/SELECT.BIN"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i)
        if(inventory(root,paths[i],NULL))return 1;
    /* Later loader generations: reported when present, never required. */
    const char *optional[]={"EFI/BOOT/BROOKV4.EFI","EFI/BOOT/BROOKV5.EFI","EFI/BOOT/BROOKT6.EFI",
        "EFI/BOOT/BROOKP5.EFI","EFI/BOOT/BROOKNEW.EFI","RECOVERY/KERNEL.ELF","RECOVERY/INITRD.IMG",
        "RECOVERY/POWER.BIN"};
    for(unsigned i=0;i<sizeof(optional)/sizeof(optional[0]);++i){
        struct stat st;
        if(fstatat(root,optional[i],&st,AT_SYMLINK_NOFOLLOW)){printf("%s absent\n",optional[i]);continue;}
        if(inventory(root,optional[i],NULL))printf("%s unreadable\n",optional[i]);
    }
    uint8_t a[128],b[128];
    if(inventory(root,"RECOVERY/GUARD0.BIN",a)||inventory(root,"RECOVERY/GUARD1.BIN",b)||
       !BrookRecoveryGuardAllowsNormal(a,sizeof(a),b,sizeof(b),argv[1]))return 1;
    close(root);puts("GUARD_INVENTORY_CLEAN");return 0;
}
