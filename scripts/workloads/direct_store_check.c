// Disposable QEMU-only direct-store persistence and application launch check.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)) { perror(#x);return 1; } } while(0)
int main(int argc,char** argv) {
    CHECK(argc==2 && (!strcmp(argv[1],"create") || !strcmp(argv[1],"verify")));
    CHECK(getuid()==1000);
    struct stat st;CHECK(!stat("/data/home",&st) && st.st_uid==1000);
    const char expected[]="Brook direct data partition survives reboot\n";
    const char* path="/data/home/.brook-direct-store-test";
    if(!strcmp(argv[1],"create")) {
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
        CHECK(write(fd,expected,sizeof(expected))==(ssize_t)sizeof(expected));
        CHECK(!fsync(fd) && !close(fd));
    } else {
        char actual[sizeof(expected)]={0};int fd=open(path,O_RDONLY);CHECK(fd>=0);
        CHECK(read(fd,actual,sizeof(actual))==(ssize_t)sizeof(actual));
        CHECK(!memcmp(actual,expected,sizeof(actual)) && !close(fd));
    }
    pid_t child=fork();CHECK(child>=0);
    if(!child) {execl("/nix/profile/bin/bash","bash","-c","exit 0",(char*)0);_exit(127);}
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts(!strcmp(argv[1],"create")?"DIRECT_STORE_CREATE_OK":"DIRECT_STORE_VERIFY_OK");return 0;
}
