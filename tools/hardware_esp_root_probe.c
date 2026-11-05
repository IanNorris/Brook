/* Guest ABI check; --bind-test modifies a disposable SSD fixture. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"

int main(int argc, char **argv) {
    if(argc>2 || (argc==2 && strcmp(argv[1],"--bind-test")))return 17;
    int esp=open("/media/system",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int child=open("/media/system/EFI",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int file=open("/media/system/BROOK.CFG",O_RDONLY|O_CLOEXEC);
    int path=open("/media/system",O_PATH|O_DIRECTORY|O_CLOEXEC);
    if(esp<0 || root<0 || child<0 || file<0 || path<0)return 1;
    int copy=dup(esp);if(copy<0)return 2;
    if(ioctl(esp,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)!=0 ||
       ioctl(child,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)!=0)return 9;
    errno=0;if(ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)!=-1 || errno!=ENODEV)return 10;
    if(ioctl(esp,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=0 ||
       ioctl(copy,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=0)return 3;
    errno=0;if(ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=ENODEV)return 4;
    errno=0;if(ioctl(child,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=ENODEV)return 5;
    errno=0;if(ioctl(file,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=ENOTTY)return 6;
    errno=0;if(ioctl(path,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=EBADF)return 7;
    errno=0;if(ioctl(-1,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=EBADF)return 8;
    /* Run only on a disposable fixture: a bind on the same FAT device must
     * not be accepted as a member of the original publication. */
    if(argc==2) {
    if(mkdir("/media/system/ESPTEST",0755))return 11;
    if(mount("/media/system/EFI","/media/system/ESPTEST",NULL,MS_BIND,NULL))return 12;
    int bound=open("/media/system/ESPTEST",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(bound<0)return 13;
    errno=0;if(ioctl(bound,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)!=-1 || errno!=ENODEV)return 14;
    close(bound);
    /* The disposable VM is torn down after this check. */
    }
    close(copy);close(path);close(file);close(child);close(root);close(esp);
    puts("SSD_ESP_IOCTL_PASS");return 0;
}
