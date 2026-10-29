#pragma once
#include <string.h>
#include "brook_system_message.h"

enum { BROOK_PENDING_MESSAGES=16 };
typedef struct BrookPendingMessages {
    BrookSystemMessage records[BROOK_PENDING_MESSAGES];
    unsigned count;
} BrookPendingMessages;
static inline int brook_message_same_subject(const BrookSystemMessage *a,const BrookSystemMessage *b) {
    if(a->victim_uid!=b->victim_uid)return 0;
    if(a->victim_incarnation || b->victim_incarnation)
        return a->victim_incarnation==b->victim_incarnation && a->victim_pid==b->victim_pid;
    return a->code==b->code && !strcmp(a->executable,b->executable);
}
static inline void brook_pending_message_add(BrookPendingMessages *queue,const BrookSystemMessage *event) {
    for(unsigned i=0;i<queue->count;++i)if(brook_message_same_subject(&queue->records[i],event)) {
        queue->records[i]=*event;return;
    }
    if(queue->count==BROOK_PENDING_MESSAGES) {
        memmove(queue->records,queue->records+1,(BROOK_PENDING_MESSAGES-1)*sizeof(*event));
        --queue->count;
    }
    queue->records[queue->count++]=*event;
}
static inline int brook_pending_message_take(BrookPendingMessages *queue,BrookSystemMessage *event) {
    if(!queue->count)return 0;
    *event=queue->records[0];--queue->count;
    memmove(queue->records,queue->records+1,queue->count*sizeof(*event));return 1;
}
