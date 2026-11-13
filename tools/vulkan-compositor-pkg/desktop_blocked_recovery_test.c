#include <assert.h>
#include <stdio.h>
#include "desktop_blocked_recovery.h"
struct Fake {long begin,status;unsigned calls,ready,aborts,waits;};
static long begin(void *p){return ((struct Fake*)p)->begin;}
static long status(void *p){struct Fake *f=p;return ++f->calls>=f->ready?f->status:1;}
static long abort_recovery(void *p){++((struct Fake*)p)->aborts;return 0;}
static void wait_recovery(void *p){++((struct Fake*)p)->waits;}
int main(void){
    struct Fake f={.ready=3};struct DesktopBlockedRecoveryOps o={&f,begin,status,abort_recovery,wait_recovery};
    assert(desktop_blocked_recovery(&o,4)==DESKTOP_BLOCKED_EXIT && f.calls==3 && f.waits==2 && !f.aborts);
    f=(struct Fake){.ready=99};assert(desktop_blocked_recovery(&o,4)==DESKTOP_BLOCKED_RETAIN && f.calls==4 && f.waits==3 && f.aborts==1);
    f=(struct Fake){.ready=1,.status=-1};assert(desktop_blocked_recovery(&o,4)==DESKTOP_BLOCKED_RETAIN && f.aborts==1);
    f=(struct Fake){.begin=-5};assert(desktop_blocked_recovery(&o,4)==DESKTOP_BLOCKED_RETAIN && f.aborts==1 && !f.calls);
    f=(struct Fake){.begin=-16};assert(desktop_blocked_recovery(&o,4)==DESKTOP_BLOCKED_RETAIN && !f.aborts && !f.calls);
    assert(desktop_blocked_recovery(&o,0)==DESKTOP_BLOCKED_RETAIN);
    puts("DESKTOP_BLOCKED_RECOVERY_PASS");return 0;
}
