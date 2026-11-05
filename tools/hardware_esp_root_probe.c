/* Guest ABI check for the boot-admitted SSD ESP root; performs no writes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"

int main(void) {
    int esp=open("/media/system",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int child=open("/media/system/EFI",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int file=open("/media/system/BROOK.CFG",O_RDONLY|O_CLOEXEC);
    int path=open("/media/system",O_PATH|O_DIRECTORY|O_CLOEXEC);
    if(esp<0 || root<0 || child<0 || file<0 || path<0)return 1;
    int copy=dup(esp);if(copy<0)return 2;
    if(ioctl(esp,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=0 ||
       ioctl(copy,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=0)return 3;
    errno=0;if(ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=ENODEV)return 4;
    errno=0;if(ioctl(child,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=ENODEV)return 5;
    errno=0;if(ioctl(file,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=ENOTTY)return 6;
    errno=0;if(ioctl(path,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=EBADF)return 7;
    errno=0;if(ioctl(-1,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)!=-1 || errno!=EBADF)return 8;
    close(copy);close(path);close(file);close(child);close(root);close(esp);
    puts("SSD_ESP_IOCTL_PASS");return 0;
}
