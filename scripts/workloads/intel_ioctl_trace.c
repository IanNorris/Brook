// Bring-up-only userspace trace of i915 calls; no kernel state is modified.
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct object {uint32_t handle,relocs;uint64_t reloc_ptr,alignment,offset,flags,reserved1,reserved2;};
int ioctl(int fd,unsigned long request,...){
    va_list ap;va_start(ap,request);void* arg=va_arg(ap,void*);va_end(ap);
    int result=(int)syscall(SYS_ioctl,fd,request,arg);int saved=errno;
    if(((request>>8)&255)=='d' && ((request&255)==0x69 || result<0)){
        dprintf(2,"ioctl nr=%02lx size=%lu result=%d errno=%d\n",request&255,(request>>16)&0x3fff,result,result<0?saved:0);
        if((request&255)==0x69 && arg && ((request>>16)&0x3fff)==sizeof(struct exec)){
            const struct exec* e=arg;dprintf(2,"exec count=%u start=%u len=%u flags=%llx ctx=%llx clips=%u fence=%llx\n",
                e->count,e->start,e->length,(unsigned long long)e->flags,(unsigned long long)e->context,e->clip_count,(unsigned long long)e->fences);
            const struct object* o=(const void*)(uintptr_t)e->objects;
            for(uint32_t i=0;o && i<e->count && i<32;++i)dprintf(2,"obj[%u] handle=%u reloc=%u align=%llx addr=%llx flags=%llx r1=%llx r2=%llx\n",
                i,o[i].handle,o[i].relocs,(unsigned long long)o[i].alignment,(unsigned long long)o[i].offset,
                (unsigned long long)o[i].flags,(unsigned long long)o[i].reserved1,(unsigned long long)o[i].reserved2);
        }
    }
    errno=saved;return result;
}
