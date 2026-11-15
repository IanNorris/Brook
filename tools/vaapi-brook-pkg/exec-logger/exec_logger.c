// exec_logger.c: LD_PRELOAD diagnostic for Brook. Logs every refused
// DRM_IOCTL_I915_GEM_EXECBUFFER2 (and every PRIME / CONTEXT_CREATE call) with
// its full object list to $BROOK_EXEC_LOG (default /tmp/brook-exec-log.<pid>).
// Observation only: the ioctl is passed through unchanged.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct eb {uint64_t objects;uint32_t count,start,length,dr1,dr4,clips;uint64_t clip_ptr,flags,rsvd1,rsvd2;};
struct obj {uint32_t handle,relocs;uint64_t reloc_ptr,alignment,offset,flags,r1,r2;};
static FILE *out;
static int libs_logged;
static int log_lib(struct dl_phdr_info *i,size_t n,void *d){(void)n;(void)d;
    if(i->dlpi_name && (strstr(i->dlpi_name,"drm") || strstr(i->dlpi_name,"libva") || strstr(i->dlpi_name,"i965")))
        fprintf(out,"LIB %s\n",i->dlpi_name);
    return 0;}
static void open_log(void){
    if (out) return;
    char path[256];const char *e=getenv("BROOK_EXEC_LOG");
    snprintf(path,sizeof path,"%s.%d",e&&*e?e:"/tmp/brook-exec-log",getpid());
    out=fopen(path,"ae");
}
int ioctl(int fd,unsigned long req,...){
    static int (*real)(int,unsigned long,...);
    if(!real)real=(int(*)(int,unsigned long,...))dlsym(RTLD_NEXT,"ioctl");
    va_list a;va_start(a,req);void *arg=va_arg(a,void*);va_end(a);
    int r=real(fd,req,arg);int err=errno;
    unsigned nr=req&255,type=(req>>8)&255;
    if(type=='d' && ((nr==0x69 && r<0) || nr==0x2d || nr==0x2e || nr==0x6d)){
        open_log();
        if(out){
            if(!libs_logged){libs_logged=1;dl_iterate_phdr(log_lib,NULL);
                const char*l=getenv("LD_LIBRARY_PATH");fprintf(out,"LD_LIBRARY_PATH=%s\n",l?l:"");}
            if(nr==0x69){
                const struct eb *e=arg;const struct obj *o=(const struct obj*)(uintptr_t)e->objects;
                fprintf(out,"EXEC fd=%d r=%d errno=%d count=%u start=%u len=%u flags=%#llx ctx=%#llx clips=%u rsvd2=%#llx\n",fd,r,err,
                    e->count,e->start,e->length,(unsigned long long)e->flags,(unsigned long long)e->rsvd1,e->clips,(unsigned long long)e->rsvd2);
                for(uint32_t i=0;o&&i<e->count&&i<128;i++)
                    fprintf(out,"  obj[%u] h=%u relocs=%u align=%#llx off=%#llx flags=%#llx r1=%#llx r2=%#llx\n",i,o[i].handle,o[i].relocs,
                        (unsigned long long)o[i].alignment,(unsigned long long)o[i].offset,(unsigned long long)o[i].flags,
                        (unsigned long long)o[i].r1,(unsigned long long)o[i].r2);
            }else{
                const uint32_t *w=arg;
                fprintf(out,"%s fd=%d r=%d errno=%d words=%u,%u,%d\n",nr==0x2d?"PRIME_EXPORT":nr==0x2e?"PRIME_IMPORT":"CTX_CREATE",fd,r,err,w[0],w[1],(int)w[2]);
            }
            fflush(out);
        }
    }
    errno=err;return r;
}
