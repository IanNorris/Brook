#define _GNU_SOURCE
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr,"FAIL %s errno=%d\n",what,errno); exit(1); }
    ++checks;
}
static int get(void) { return prctl(PR_GET_NO_NEW_PRIVS,0L,0L,0L,0L); }
static void bad(int option, unsigned long a, unsigned long b, unsigned long c, unsigned long d) {
    errno=0;
    check(prctl(option,a,b,c,d)==-1 && errno==EINVAL,"invalid prctl arguments");
}
static void reap(pid_t pid) {
    int status;
    check(pid>0,"fork");
    check(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,"child completion");
}
static int gate[2], result[2];
static void *sibling(void *unused) {
    (void)unused;
    char c='x'; int value;
    /* This task sets NNP without changing its creator. */
    if (prctl(PR_SET_NO_NEW_PRIVS,1L,0L,0L,0L)!=0) return (void*)1;
    value=get();
    if (write(result[1],&value,sizeof value)!=(ssize_t)sizeof value) return (void*)1;
    if (read(gate[0],&c,1)!=1) return (void*)1;
    return get()==1 ? NULL : (void*)1;
}
static void *old_sibling(void *unused) {
    (void)unused;
    char c='x'; int value=get();
    if (write(result[1],&value,sizeof value)!=(ssize_t)sizeof value) return (void*)1;
    if (read(gate[0],&c,1)!=1) return (void*)1;
    return get()==0 ? NULL : (void*)1;
}
static void *new_sibling(void *unused) {
    (void)unused;
    return get()==1 ? NULL : (void*)1;
}
static void join(pthread_t t) {
    void *value=(void*)1;
    check(pthread_join(t,&value)==0 && value==NULL,"thread completion and private flag");
}
int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"--exec-child")) {
        check(get()==1,"successful exec preserves NNP");
        check(getuid()==1000 && geteuid()==1000,"exec retains unprivileged identity");
        return 0;
    }
    int inherited = argc==2 && !strcmp(argv[1],"--already-restricted");
    check(get()==inherited,"initial task NNP matches requested fixture");
    bad(PR_SET_NO_NEW_PRIVS,0,0,0,0);
    bad(PR_SET_NO_NEW_PRIVS,2,0,0,0);
    bad(PR_SET_NO_NEW_PRIVS,1,1,0,0);
    bad(PR_SET_NO_NEW_PRIVS,1,0,1,0);
    bad(PR_SET_NO_NEW_PRIVS,1,0,0,1);
    bad(PR_GET_NO_NEW_PRIVS,1,0,0,0);
    bad(PR_GET_NO_NEW_PRIVS,0,1,0,0);
    bad(PR_GET_NO_NEW_PRIVS,0,0,1,0);
    bad(PR_GET_NO_NEW_PRIVS,0,0,0,1);
    check(get()==inherited,"rejected calls preserve flag");
    pid_t pid=fork();
    if (!pid) _exit(get()==inherited ? 0 : 1);
    reap(pid);
    check(pipe(gate)==0 && pipe(result)==0,"sibling synchronization pipes");
    pthread_t t;
    if (!inherited) {
    check(pthread_create(&t,NULL,sibling,NULL)==0,"create independent sibling");
    int value=-1;
    check(read(result[0],&value,sizeof value)==(ssize_t)sizeof value && value==1,"sibling set NNP");
    check(get()==0,"sibling cannot set creator NNP");
    check(write(gate[1],"x",1)==1,"release first sibling");
    join(t);
    check(pthread_create(&t,NULL,old_sibling,NULL)==0,"create sibling before parent transition");
    check(read(result[0],&value,sizeof value)==(ssize_t)sizeof value && value==0,"old sibling starts unrestricted");
    }
    check(prctl(PR_SET_NO_NEW_PRIVS,1L,0L,0L,0L)==0 && get()==1,"set and get NNP");
    check(prctl(PR_SET_NO_NEW_PRIVS,1L,0L,0L,0L)==0,"repeated set succeeds");
    bad(PR_SET_NO_NEW_PRIVS,0,0,0,0);
    check(get()==1,"NNP cannot be cleared");
    if (!inherited) {
        check(write(gate[1],"x",1)==1,"release old sibling");
        join(t);
    }
    check(pthread_create(&t,NULL,new_sibling,NULL)==0,"new thread inherits creator NNP");
    join(t);
    pid=fork();
    if (!pid) _exit(get()==1 ? 0 : 1);
    reap(pid);
    errno=0;
    char *missing[]={"/no-such-nnp-program",NULL};
    execv(missing[0],missing);
    check(errno==ENOENT && get()==1,"failed exec preserves NNP");
    pid=fork();
    if (!pid) {
        if (getuid()==0 && setgid(1000)!=0) _exit(10);
        if (getuid()==0 && setuid(1000)!=0) _exit(11);
        char *args[]={argv[0],"--exec-child",NULL};
        execv(argv[0],args);
        _exit(12);
    }
    reap(pid);
    close(gate[0]); close(gate[1]); close(result[0]); close(result[1]);
    printf("NNP PASS %d checks (plus exec-child); initial=%d, transition/isolation %s\n",checks,inherited,inherited ? "SKIPPED (host already restricted)" : "tested");
    return 0;
}
