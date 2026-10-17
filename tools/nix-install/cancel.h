/* Cooperative Gallery cancellation. Only SIGUSR1 is the request protocol;
 * SIGKILL and terminal process-group interruption retain ordinary semantics. */
#ifndef BROOK_INSTALL_CANCEL_H
#define BROOK_INSTALL_CANCEL_H
#include <signal.h>
#include <time.h>
static volatile sig_atomic_t install_cancel_requested;
static int install_commit_started;
static void install_cancel_signal(int signo) {(void)signo;install_cancel_requested=1;}
static int install_cancel_enable(void) {
    struct sigaction action={0};action.sa_handler=install_cancel_signal;action.sa_flags=SA_RESTART;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGUSR1,&action,NULL)<0)return -1;
    sigset_t unblock;sigemptyset(&unblock);sigaddset(&unblock,SIGUSR1);
    return sigprocmask(SIG_UNBLOCK,&unblock,NULL);
}
static int install_cancel_status(int result) {
    if(install_cancel_requested&&!install_commit_started) {
        fprintf(stderr,"Install cancelled before profile update. Completed store downloads remain cached.\n");
        return 130;
    }
    return result;
}
static int install_commit_begin(sigset_t *previous) {
    sigset_t blocked,pending;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);
    if(sigprocmask(SIG_BLOCK,&blocked,previous)<0)return 1;
    if(sigpending(&pending)<0) {sigprocmask(SIG_SETMASK,previous,NULL);return 1;}
    if(install_cancel_requested||sigismember(&pending,SIGUSR1)) {
        sigprocmask(SIG_SETMASK,previous,NULL);return 130;
    }
    /* Requests after this boundary cannot interrupt profile publication. */
    install_commit_started=1;
    puts("Updating package profile. Finishing this update before stopping.");
    return 0;
}
static int install_commit_end(const sigset_t *previous,int result) {
    if(sigprocmask(SIG_SETMASK,previous,NULL)<0)return 1;
    return result;
}
static int install_run_fetch(char *const argv[]) {
    if(install_cancel_requested)return 130;
    int channel[2];if(pipe(channel)<0){perror("cancellation pipe");return 1;}
    /* Closed standard streams must not turn the private control channel into
     * stdin/out/err. Keep both ends private across unrelated execs. */
    for(int i=0;i<2;i++) {
        int private_fd=fcntl(channel[i],F_DUPFD_CLOEXEC,3);
        if(private_fd<0){perror("cancellation descriptor");close(channel[0]);close(channel[1]);return 1;}
        close(channel[i]);channel[i]=private_fd;
    }
    if(fcntl(channel[0],F_SETFL,O_NONBLOCK)<0) {close(channel[0]);close(channel[1]);return 1;}
    pid_t pid=fork();
    if(pid<0){close(channel[0]);close(channel[1]);return 1;}
    if(!pid) {
        close(channel[1]);
        if(fcntl(channel[0],F_SETFD,0)<0)_exit(127);
        char number[32];snprintf(number,sizeof(number),"%d",channel[0]);
        if(setenv("BROOK_NIX_CANCEL_FD",number,1)<0)_exit(127);
        execv(argv[0],argv);perror("exec nix-fetch");_exit(127);
    }
    close(channel[0]);int status=0;
    for(;;) {
        /* EOF is cancellation, including parent death. Never kill an archive
         * pipeline midway through extraction or leave its children unawaited. */
        if(install_cancel_requested&&channel[1]>=0){close(channel[1]);channel[1]=-1;}
        pid_t result=waitpid(pid,&status,WNOHANG);
        if(result==pid)break;
        if(result<0&&errno!=EINTR){perror("wait nix-fetch");if(channel[1]>=0)close(channel[1]);return 1;}
        struct timespec pause={0,50000000};nanosleep(&pause,NULL);
    }
    if(channel[1]>=0)close(channel[1]);
    int result=WIFEXITED(status)?WEXITSTATUS(status):128;
    if(result==130)install_cancel_requested=1;
    return install_cancel_requested?130:result;
}
#endif
