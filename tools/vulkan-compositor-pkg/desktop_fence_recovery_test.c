#include "desktop_fence_recovery.h"
#include <stdio.h>
struct Fixture {long observed,released;int retired,observes,retires,releases;};
static long observe(void *p){struct Fixture *f=p;++f->observes;return f->observed;}
static int retire(void *p){struct Fixture *f=p;++f->retires;return f->retired;}
static long release(void *p){struct Fixture *f=p;++f->releases;return f->released;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FENCE_RECOVERY_FAIL line=%d\n",__LINE__);return 1;}}while(0)
int main(void){
    struct Fixture f={0};struct DesktopFenceRecoveryOps ops={&f,observe,retire,release};
    CHECK(desktop_fence_recovery_step(&ops)==DESKTOP_FENCE_PENDING && !f.retires && !f.releases);
    f.observed=2;CHECK(desktop_fence_recovery_step(&ops)==DESKTOP_FENCE_PENDING && !f.retires && !f.releases);
    f.observed=1;CHECK(desktop_fence_recovery_step(&ops)==DESKTOP_FENCE_PENDING && f.retires==1 && !f.releases);
    f.retired=1;CHECK(desktop_fence_recovery_step(&ops)==DESKTOP_FENCE_RECOVERED && f.releases==1);
    f.released=-1;CHECK(desktop_fence_recovery_step(&ops)==DESKTOP_FENCE_RETAIN && f.releases==2);
    ops.observe=NULL;CHECK(desktop_fence_recovery_step(&ops)==DESKTOP_FENCE_RETAIN);
    puts("DESKTOP_FENCE_RECOVERY_PASS");return 0;
}
