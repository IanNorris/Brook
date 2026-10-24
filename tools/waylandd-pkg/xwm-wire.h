#ifndef BROOK_XWM_WIRE_H
#define BROOK_XWM_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

/* Private inherited stream socket only. Fixed framing handles split/coalesced
 * AF_UNIX writes without transferring pointers or client-supplied authority.
 * Full queues are fatal to this Xwayland generation, never silently dropped.
 */
enum {
    BXWM_VERSION=1,
    BXWM_READY=1, BXWM_WINDOW=2, BXWM_DESTROY=3, BXWM_ERROR=4,
    BXWM_BEGIN_MOVE=5, BXWM_MAXIMIZE=6, BXWM_MINIMIZE=7, BXWM_BEGIN_RESIZE=8,
    BXWM_CONFIGURE=16, BXWM_FOCUS=17, BXWM_CLOSE=18,
    BXWM_MAPPED=1u<<0, BXWM_OVERRIDE=1u<<1, BXWM_INPUT=1u<<2,
    BXWM_CLIENT_CHROME=1u<<3,
    BXWM_MAXIMIZED=1u<<4, BXWM_MINIMIZED=1u<<5,
    BXWM_QUEUE=64
};
struct bxwm_message {
    uint32_t version,type;
    uint64_t generation,identity,serial,parent_identity,configure_serial;
    uint32_t window,parent;
    int32_t x,y;
    uint32_t width,height,flags,reserved;
    char title[64];
};
_Static_assert(sizeof(struct bxwm_message)==144,"private XWM framing");
struct bxwm_stream {
    struct bxwm_message incoming,queue[BXWM_QUEUE];
    size_t received,sent;
    unsigned head,count;
};
static int bxwm_enqueue(struct bxwm_stream *stream,const struct bxwm_message *message) {
    if (stream->count==BXWM_QUEUE) { errno=ENOBUFS;return 0; }
    stream->queue[(stream->head+stream->count)%BXWM_QUEUE]=*message;++stream->count;return 1;
}
/* Returns 1 for fully flushed, 0 for backpressure, -1 for closed/broken stream. */
static int bxwm_flush(int fd,struct bxwm_stream *stream) {
    while (stream->count) {
        const char *bytes=(const char *)&stream->queue[stream->head];
        ssize_t n=send(fd,bytes+stream->sent,sizeof(struct bxwm_message)-stream->sent,MSG_DONTWAIT|MSG_NOSIGNAL);
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
        if (n<=0) return -1;
        stream->sent+=(size_t)n;
        if (stream->sent==sizeof(struct bxwm_message)) {
            stream->sent=0;stream->head=(stream->head+1)%BXWM_QUEUE;--stream->count;
        }
    }
    return 1;
}
/* Returns 1 for a complete message, 0 for incomplete/EAGAIN, -1 for EOF/error. */
static int bxwm_receive(int fd,struct bxwm_stream *stream,struct bxwm_message *message) {
    while (stream->received<sizeof(struct bxwm_message)) {
        char *bytes=(char *)&stream->incoming;
        ssize_t n=recv(fd,bytes+stream->received,sizeof(struct bxwm_message)-stream->received,MSG_DONTWAIT);
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
        if (n<=0) return -1;
        stream->received+=(size_t)n;
    }
    *message=stream->incoming;stream->received=0;return 1;
}
static int bxwm_valid(const struct bxwm_message *message,uint64_t generation,int from_helper) {
    if (message->version!=BXWM_VERSION || !generation || message->generation!=generation || message->reserved)
        return 0;
    if (from_helper && (message->type==BXWM_READY || message->type==BXWM_ERROR))
        return message->identity==0 && message->serial==0;
    if (!message->window || !message->identity) return 0;
    if (from_helper && (message->type==BXWM_BEGIN_MOVE || message->type==BXWM_MINIMIZE)) return 1;
    if (from_helper && message->type==BXWM_MAXIMIZE) return message->x>=0 && message->x<=2;
    if (from_helper && message->type==BXWM_BEGIN_RESIZE) return message->x>=0 && message->x<=7;
    if (from_helper && message->type==BXWM_DESTROY) return 1;
    if (from_helper && message->type==BXWM_WINDOW)
        return message->width && message->height && message->width<=65535 && message->height<=65535 &&
            !(message->flags&~(BXWM_MAPPED|BXWM_OVERRIDE|BXWM_INPUT|BXWM_CLIENT_CHROME|BXWM_MAXIMIZED|BXWM_MINIMIZED)) &&
            message->title[sizeof(message->title)-1]==0;
    if (!from_helper && message->type==BXWM_CONFIGURE)
        return message->width && message->height && message->width<=65535 && message->height<=65535 &&
            message->x>=-32768 && message->x<=32767 && message->y>=-32768 && message->y<=32767;
    if (!from_helper && (message->type==BXWM_FOCUS || message->type==BXWM_CLOSE)) return 1;
    return 0;
}
#endif
