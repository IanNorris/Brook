#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void){
 const char *path="/media/system/GATEQA.TXT", value[]="brook-ahci-gate-smoke\n"; char out[sizeof(value)]={0};
 int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600); if(fd<0){perror("open");return 1;}
 if(write(fd,value,sizeof(value)-1)!=(ssize_t)sizeof(value)-1 || fsync(fd) || lseek(fd,0,SEEK_SET)<0 || read(fd,out,sizeof(value)-1)!=(ssize_t)sizeof(value)-1 || memcmp(value,out,sizeof(value)-1) || close(fd)){perror("file");return 2;}
 puts("AHCI_GATE_IO_SMOKE_OK");return 0;
}
