// Feed an explicit GPT script to the staged, unmodified util-linux binary.
// This helper is intended only for the disposable QEMU block-tools fixture.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fd=open("/boot/layout.txt",O_RDONLY);
    if(fd<0 || dup2(fd,0)<0) {perror("layout");return 2;}
    close(fd);
    execl("/boot/LIB/ld-linux-x86-64.so.2","ld-linux-x86-64.so.2",
          "--library-path","/boot/LIB","/boot/sfdisk","--no-reread",
          "--no-tell-kernel","/dev/brook-test-disk",(char*)0);
    perror("exec sfdisk");return 2;
}
