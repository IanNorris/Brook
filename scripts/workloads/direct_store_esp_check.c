// Disposable QEMU check: the direct data lease must not block EFI writes.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { perror(#x);return 1; } } while(0)
int main(void) {
    const char data[]="EFI writes remain available\n";char actual[sizeof(data)]={0};
    const char* path="/media/system/DIRECTQA.TXT";
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);CHECK(fd>=0);
    CHECK(write(fd,data,sizeof(data))==(ssize_t)sizeof(data));
    CHECK(!fsync(fd) && !close(fd));
    fd=open(path,O_RDONLY);CHECK(fd>=0);
    CHECK(read(fd,actual,sizeof(actual))==(ssize_t)sizeof(actual));
    CHECK(!memcmp(actual,data,sizeof(data)) && !close(fd));
    puts("DIRECT_ESP_WRITE_OK");return 0;
}
