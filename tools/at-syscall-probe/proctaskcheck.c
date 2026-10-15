#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"PROCTASKCHECK FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); ++failures; } } while(0)
static atomic_int tid, release_worker, worker_ok;
static int status_matches(const char *path, int expected_pid, int expected_tgid, int threads)
{
    char buf[2048]; int fd=open(path,O_RDONLY);
    if(fd<0) return 0;
    ssize_t n=read(fd,buf,sizeof(buf)-1); close(fd);
    if(n<=0) return 0;
    buf[n]=0;
    char *p=strstr(buf,"\nPid:\t"), *g=strstr(buf,"\nTgid:\t"), *t=strstr(buf,"\nThreads:\t");
    return p&&g&&t&&atoi(p+6)==expected_pid&&atoi(g+7)==expected_tgid&&atoi(t+10)==threads;
}
static int enumerate(DIR *dir, int leader, int worker, int expected)
{
    int count=0, seen_leader=0, seen_worker=0;
    struct dirent *de;
    errno=0;
    while((de=readdir(dir))) {
        if(de->d_name[0]=='.') continue;
        char *end; long value=strtol(de->d_name,&end,10);
        if(*end||value<=0) return -1;
        ++count;
        if(value==leader) ++seen_leader;
        else if(value==worker) ++seen_worker;
        else return -1;
    }
    if(errno && errno!=ENOENT && errno!=ESRCH) return -1;
    return count==expected && (!expected || seen_leader==1) && (expected!=2 || seen_worker==1);
}
static void *worker_main(void *unused)
{
    (void)unused;
    int me=(int)syscall(SYS_gettid);
    atomic_store(&tid,me);
    struct stat a,b;
    char path[128]; snprintf(path,sizeof(path),"/proc/%d/task/%d",getpid(),me);
    int ok=stat("/proc/thread-self",&a)==0&&stat(path,&b)==0&&a.st_ino==b.st_ino;
    ok=ok&&status_matches("/proc/thread-self/status",me,getpid(),2);
    ok=ok&&status_matches("/proc/self/status",getpid(),getpid(),2);
    atomic_store(&worker_ok,ok);
    while(!atomic_load(&release_worker)) usleep(1000);
    return NULL;
}
int main(void)
{
    int procfd=open("/proc",O_RDONLY|O_DIRECTORY);
    CHECK(procfd>=0);
    struct stat st;
    CHECK(fstatat(procfd,"self/task/",&st,0)==0&&S_ISDIR(st.st_mode)&&st.st_nlink==3);
    pthread_t thread;
    int rc=pthread_create(&thread,NULL,worker_main,NULL);
    CHECK(rc==0); if(rc) return 1;
    for(int i=0;i<5000&&!atomic_load(&tid);++i) usleep(1000);
    int worker=atomic_load(&tid); CHECK(worker>0&&worker!=getpid());
    CHECK(fstatat(procfd,"self/task/",&st,0)==0&&st.st_nlink==4);
    DIR *dir=opendir("/proc/self/task"); CHECK(dir!=NULL);
    if(dir) { CHECK(enumerate(dir,getpid(),worker,2)); closedir(dir); }
    char path[128]; snprintf(path,sizeof(path),"/proc/self/task/%d/status",worker);
    CHECK(status_matches(path,worker,getpid(),2));
    snprintf(path,sizeof(path),"/proc/self/task/%d",worker);
    CHECK(stat(path,&st)==0&&S_ISDIR(st.st_mode));
    CHECK(stat("/proc/selfish/task",&st)<0&&errno==ENOENT);
    CHECK(stat("/proc/4294967297/task",&st)<0&&errno==ENOENT);
    atomic_store(&release_worker,1); CHECK(pthread_join(thread,NULL)==0);
    CHECK(atomic_load(&worker_ok));
    for(int i=0;i<2000;++i) { if(stat(path,&st)<0&&errno==ENOENT) break; usleep(1000); }
    CHECK(stat(path,&st)<0&&errno==ENOENT);
    CHECK(fstatat(procfd,"self/task/",&st,0)==0&&st.st_nlink==3);

    // A live foreign group must never be enumerated or accepted as our task.
    // Keep its task directory open across exit and possible PID reuse.
    int pipefd[2]; rc=pipe(pipefd); CHECK(rc==0); if(rc) return 1;
    pid_t child=fork(); CHECK(child>=0); if(child<0) return 1;
    if(child==0) { close(pipefd[1]); char c; (void)read(pipefd[0],&c,1); _exit(0); }
    close(pipefd[0]);
    snprintf(path,sizeof(path),"/proc/self/task/%d",child);
    CHECK(stat(path,&st)<0&&errno==ENOENT);
    snprintf(path,sizeof(path),"/proc/%d/task",child);
    DIR *old=opendir(path); CHECK(old!=NULL);
    ino_t old_ino=0;
    if(old) { CHECK(fstat(dirfd(old),&st)==0); old_ino=st.st_ino; CHECK(enumerate(old,child,0,1)); }
    close(pipefd[1]); int status; CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status));
    int reused=0;
    for(int round=0;round<32;++round) {
        rc=pipe(pipefd); CHECK(rc==0); if(rc) return 1;
        pid_t next=fork(); CHECK(next>=0); if(next<0) return 1;
        if(next==0) { close(pipefd[1]); char c; (void)read(pipefd[0],&c,1); _exit(0); }
        close(pipefd[0]);
        if(next==child) reused=1;
        if(old) {
            errno=0; int stat_rc=fstat(dirfd(old),&st);
            CHECK((stat_rc==0&&st.st_ino==old_ino)||(stat_rc<0&&(errno==ENOENT||errno==ESRCH||errno==EBADF)));
            rewinddir(old); CHECK(enumerate(old,0,0,0));
        }
        close(pipefd[1]); CHECK(waitpid(next,&status,0)==next&&WIFEXITED(status));
    }
    if(old) closedir(old);
    close(procfd);
    if(failures) return 1;
    printf("PROCTASKCHECK PASS live=2 single=1 stale_dir=32 observed_reuse=%d\n",reused);
    return 0;
}
