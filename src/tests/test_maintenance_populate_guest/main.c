// Populate a disposable QEMU EFI partition from the maintenance USB.
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(const char* app,const char* a,const char* b) {
    pid_t child=fork();if(child<0)return -1;
    if(child==0) {
        if(b)execl("/boot/LIB/ld-linux-x86-64.so.2","ld-linux-x86-64.so.2",
                   "--argv0",app,"--library-path","/boot/LIB","/boot/mtools",
                   "-i","/dev/brook-test-disk1",a,b,(char*)0);
        else execl("/boot/LIB/ld-linux-x86-64.so.2","ld-linux-x86-64.so.2",
                   "--argv0",app,"--library-path","/boot/LIB","/boot/mtools",
                   "-i","/dev/brook-test-disk1",a,(char*)0);
        _exit(127);
    }
    int status=0;
    return waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0?0:-1;
}
int main(void) {
    if(run("mmd","::/EFI",0) || run("mmd","::/EFI/BOOT",0) || run("mmd","::/KERNEL",0) ||
       run("mcopy","/boot/EFIQA.EFI","::/EFI/BOOT/BOOTX64.EFI") ||
       run("mcopy","/media/usb/KERNEL/BROOK.ELF","::/KERNEL/BROOK.ELF") ||
       run("mcopy","/media/usb/INITRD.IMG","::/INITRD.IMG") ||
       run("mcopy","/boot/BROOKQA.CFG","::/BROOK.CFG")) {
        fputs("EFI population failed\n",stderr);return 1;
    }
    int fd=open("/dev/brook-test-disk1",O_RDWR);
    if(fd<0 || fsync(fd) || close(fd)) {perror("EFI flush");return 1;}
    puts("MAINTENANCE_EFI_POPULATED");return 0;
}
