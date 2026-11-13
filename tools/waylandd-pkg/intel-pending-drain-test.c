#include <assert.h>
#include <stdio.h>
#include "intel_pending_drain.h"
struct State {unsigned pending,polls,waits,finishAfter;};
static unsigned count(void *p){return ((struct State*)p)->pending;}
static void poll_pending(void *p){struct State *s=p;if(++s->polls==s->finishAfter)s->pending=0;}
static void wait_pending(void *p){++((struct State*)p)->waits;}
int main(void){
    struct State s={0};struct IntelPendingDrainOps ops={&s,count,poll_pending,wait_pending};
    assert(intel_pending_drain(&ops,5) && !s.polls && !s.waits);
    s=(struct State){1,0,0,3};assert(intel_pending_drain(&ops,5) && s.polls==3 && s.waits==2);
    s=(struct State){1,0,0,9};assert(!intel_pending_drain(&ops,5) && s.polls==5 && s.waits==5);
    assert(!intel_pending_drain(NULL,5));puts("INTEL_PENDING_DRAIN_PASS");return 0;
}
