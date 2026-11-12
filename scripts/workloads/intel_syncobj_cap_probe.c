// Diagnostic shim: make DRM_CAP_SYNCOBJ visible only to this test process.
// This does not implement missing syncobj operations or alter kernel state.
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
struct cap {uint64_t capability,value;};
struct getparam {int32_t param,pad;uint64_t value;};
struct sync_wait {uint64_t handles;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct fence {uint32_t handle,flags;};
int ioctl(int fd,unsigned long request,...){
    va_list ap;va_start(ap,request);void *arg=va_arg(ap,void*);va_end(ap);
    if(((request>>8)&255)=='d' && (request&255)==0x69 && arg){
        struct exec *e=arg;
        dprintf(2,"EXEC count=%u start=%u len=%u flags=%llx context=%llu fences=%u\n",e->count,e->start,e->length,(unsigned long long)e->flags,(unsigned long long)e->context,e->clip_count);
        if(e->count<=32){struct object *o=(void*)(uintptr_t)e->objects;
            for(unsigned i=0;i<e->count;i++)dprintf(2," EXEC_OBJECT handle=%u offset=%llx flags=%llx\n",o[i].handle,(unsigned long long)o[i].offset,(unsigned long long)o[i].flags);}
        if((e->flags&(1u<<19)) && e->clip_count<=16){struct fence *f=(void*)(uintptr_t)e->clips;
            for(unsigned i=0;i<e->clip_count;i++)dprintf(2," EXEC_FENCE handle=%u flags=%u\n",f[i].handle,f[i].flags);}
    }
    struct timespec before={0},after={0};
    if(((request>>8)&255)=='d' && (request&255)==0xc3 && arg)clock_gettime(CLOCK_MONOTONIC,&before);
    int result=(int)syscall(SYS_ioctl,fd,request,arg);int saved=errno;
    if(((request>>8)&255)=='d' && (request&255)==0xc3 && arg){
        clock_gettime(CLOCK_MONOTONIC,&after);struct sync_wait *w=arg;
        dprintf(2,"SYNC_WAIT count=%u flags=%x first=%u first_handle=%u elapsed_ns=%lld\n",w->count,w->flags,w->first,w->count?*(uint32_t*)(uintptr_t)w->handles:0,(long long)(after.tv_sec-before.tv_sec)*1000000000ll+after.tv_nsec-before.tv_nsec);
    }
    if(((request>>8)&255)=='d' && (request&255)==0x0c && ((request>>16)&0x3fff)==sizeof(struct cap) && arg){
        struct cap *c=arg;if(!result && c->capability==0x13 && !c->value){c->value=1;dprintf(2,"PROBE_ONLY DRM_CAP_SYNCOBJ=1\n");}
    }
    if(((request>>8)&255)=='d' && (request&255)==0x46 && ((request>>16)&0x3fff)==sizeof(struct getparam) && arg){
        struct getparam *p=arg;if(!result && (p->param==45 || p->param==55) && p->value && !*(int32_t*)(uintptr_t)p->value){
            *(int32_t*)(uintptr_t)p->value=1;
            dprintf(2,"PROBE_ONLY %s=1\n",p->param==45?"HAS_EXEC_CAPTURE":"HAS_EXEC_TIMELINE_FENCES");
        }
    }
    if(((request>>8)&255)=='d' && (result<0 || (request&255)==0x69 || ((request&255)>=0xbf && (request&255)<=0xcf)))
        dprintf(2,"ioctl nr=%02lx size=%lu result=%d errno=%d\n",request&255,(request>>16)&0x3fff,result,result<0?saved:0);
    errno=saved;return result;
}
