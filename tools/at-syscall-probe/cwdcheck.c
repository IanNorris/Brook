#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#define CHECK(x) do { if(!(x)){perror(#x); return 1;} } while(0)
int main(int argc,char **argv) {
    char root[256],absolute[256];
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    snprintf(root,sizeof(root),"%s/cwdcheck-%d-%ld",argc>1?argv[1]:"/data/tmp",getpid(),now.tv_nsec);
    CHECK(mkdir(root,0755)==0);
    CHECK(chdir(root)==0);
    CHECK(mkdir("child",0755)==0);
    CHECK(mkdirat(AT_FDCWD,"at-child",0755)==0);
    snprintf(absolute,sizeof(absolute),"%s/child",root);
    struct stat st;
    CHECK(stat(absolute,&st)==0&&S_ISDIR(st.st_mode));
    snprintf(absolute,sizeof(absolute),"%s/at-child",root);
    CHECK(stat(absolute,&st)==0&&S_ISDIR(st.st_mode));
    int file=open("child/file",O_CREAT|O_WRONLY,0644);
    CHECK(file>=0); CHECK(close(file)==0);
    CHECK(unlink("child/file")==0);
    CHECK(access("child/file",F_OK)==-1);
    sync();
    puts("CWDCHECK PASS: relative mkdir, mkdirat(AT_FDCWD), and unlink");
    return 0;
}
