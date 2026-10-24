#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int byte(int fd,char* value,int writing) {
    ssize_t n;do {n=writing?write(fd,value,1):read(fd,value,1);}while(n<0 && errno==EINTR);
    return n==1;
}
int main(int argc,char** argv) {
    if(argc==5 && !strcmp(argv[1],"--child")) {
        int ready=atoi(argv[2]),release=atoi(argv[3]),fail=atoi(argv[4]);
        pid_t expected=getppid(),grand=fork();
        if(grand<0)_exit(90);
        if(grand>0)_exit(0);
        alarm(15);
        for(int n=0;getppid()!=expected && n<1000;++n)usleep(10000);
        if(getppid()!=expected)_exit(91);
        char value='a';
        if(!byte(ready,&value,1) || !byte(release,&value,0))_exit(92);
        _exit(getppid()==expected?(fail?17:0):93);
    }
    if(argc!=2)return 2;
    alarm(40);
    for(int fail=0;fail<2;++fail) {
        int ready[2],release[2];if(pipe(ready)||pipe(release))return 3;
        pid_t reaper=fork();
        if(reaper==0) {
            char r[32],g[32],f[8];snprintf(r,sizeof(r),"%d",ready[1]);snprintf(g,sizeof(g),"%d",release[0]);snprintf(f,sizeof(f),"%d",fail);
            close(ready[0]);close(release[1]);
            execl(argv[1],argv[1],"BrookPublicOrphanControl","--",argv[0],"--child",r,g,f,(char*)NULL);_exit(94);
        }
        close(ready[1]);close(release[0]);char value;int status;
        int ok=reaper>0 && byte(ready[0],&value,0) && value=='a' &&
            waitpid(reaper,&status,WNOHANG)==0 && byte(release[1],&value,1);
        close(ready[0]);close(release[1]);
        pid_t got;do {got=waitpid(reaper,&status,0);}while(got<0 && errno==EINTR);
        if(!ok || got!=reaper || !WIFEXITED(status) || WEXITSTATUS(status)!=fail) {
            printf("{\"reaper_pass\":false,\"grandchild_failure\":%d,\"wait_status\":%d}\n",fail,status);return 1;
        }
    }
    alarm(0);
    puts("{\"reaper_pass\":true,\"cases\":2,\"scope\":\"real helper stays for adopted grandchild and propagates success/failure\"}");return 0;
}
