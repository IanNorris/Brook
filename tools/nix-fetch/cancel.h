#ifndef BROOK_FETCH_CANCEL_H
#define BROOK_FETCH_CANCEL_H
#include <limits.h>
/* Inherited private read end. EOF asks us to stop between completed operations.
 * CLOEXEC prevents transfer/extraction helpers retaining the descriptor. */
static int fetch_cancel_fd=-1,fetch_was_cancelled;
static int fetch_cancel_init(void) {
    const char *setting=getenv("BROOK_NIX_CANCEL_FD");if(!setting)return 0;
    char *end;errno=0;long number=strtol(setting,&end,10);
    if(errno||!*setting||*end||number<3||number>INT_MAX){errno=EINVAL;return -1;}
    fetch_cancel_fd=(int)number;struct stat st;
    if(fstat(fetch_cancel_fd,&st)<0)return -1;
    if(!S_ISFIFO(st.st_mode)){errno=EINVAL;return -1;}
    if(fcntl(fetch_cancel_fd,F_SETFL,O_NONBLOCK)<0||fcntl(fetch_cancel_fd,F_SETFD,FD_CLOEXEC)<0)return -1;
    return 0;
}
static int fetch_cancelled(void) {
    if(fetch_was_cancelled||fetch_cancel_fd<0)return fetch_was_cancelled;
    char byte;ssize_t n=read(fetch_cancel_fd,&byte,1);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return 0;
    /* EOF, an explicit byte or a broken control channel all stop new work. */
    fetch_was_cancelled=1;
    fprintf(stderr,"nix-fetch: cancellation requested; current completed downloads stay cached.\n");
    return 1;
}
#endif
