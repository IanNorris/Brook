// brook_i915_shim.cpp - host-side LD_PRELOAD harness: Brook's i915 render
// ioctl semantics for unmodified userspace (VA-API phase 2a).
//
// The shim opens the host's real render node only to obtain a genuine DRM
// character-device fd (libva and libdrm inspect st_rdev), then intercepts
// EVERY ioctl and mmap on that fd (and on its dups) and serves them with
// Brook's own kernel code: intel_bdw_ioctl.cpp, intel_bdw_file.cpp,
// intel_bdw_buffer.cpp and intel_bdw_exec_snapshot.cpp built for the host.
// Nothing ever reaches the host GPU. Buffer pages live in one sparse memfd
// arena, so mmap returns real shared mappings of the same pages the "kernel"
// side sees.
//
// Execution is not emulated: an admitted execbuffer (Brook's full decode:
// NO_RELOC, softpin, object/flag/range validation, page and table limits)
// is retired at once as completed and its fences signalled. Decoded video is
// therefore garbage; the harness tests the ioctl contract, not the pixels.
//
// Environment:
//   BROOK_SHIM_NODE  render node to wrap (default /dev/dri/renderD128)
//   BROOK_SHIM_BSD   HAS_BSD answer: 1 (default) or 0
//   BROOK_SHIM_LOG   path for a per-ioctl log (default: stderr summary only)
#define _GNU_SOURCE 1
#include "intel_bdw_video.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_file.h"
#include "intel_bdw_ioctl.h"
#include "intel_bdw_ioctl_trace.h"
#include "intel_bdw_refusal.h"
#include "intel_bdw_exec_snapshot.h"
#include "intel_vtd_observe.h"
#include "intel_vtd_probe_policy.h"
#include "memory/mapping_ranges.h"
#include "memory/virtual_memory.h"
#include "sync/kmutex.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

using namespace brook;

// ---- kernel environment stubs (host) ----
namespace {
constexpr uint64_t ArenaBytes=uint64_t{8}<<30;   // sparse
constexpr uint64_t DmaBase=0x100000000ull;       // fake DMA = arena offset + base (< 16 GiB)
int arenaFd=-1;uint8_t* arenaBase=nullptr;uint64_t arenaUsed=0;pthread_mutex_t arenaLock=PTHREAD_MUTEX_INITIALIZER;
struct Freed {uint64_t offset,pages;};Freed freedRanges[4096];unsigned freedCount=0;
void ArenaInit(){
    if(arenaBase)return;
    arenaFd=int(syscall(SYS_memfd_create,"brook-i915-arena",0));
    if(arenaFd<0 || ftruncate(arenaFd,off_t(ArenaBytes))){perror("brook-shim: arena");abort();}
    void* p=mmap(nullptr,ArenaBytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,arenaFd,0);
    if(p==MAP_FAILED){perror("brook-shim: arena map");abort();}
    arenaBase=static_cast<uint8_t*>(p);
}
}
namespace brook {
extern "C" void* kmalloc(uint64_t n){return malloc(n?n:1);}
extern "C" void kfree(void* p){free(p);}
VirtualAddress VmmAllocPages(uint64_t n,uint64_t,MemTag,uint16_t){
    pthread_mutex_lock(&arenaLock);ArenaInit();
    uint64_t offset=~uint64_t{0};
    for(unsigned i=0;i<freedCount;++i)if(freedRanges[i].pages==n){offset=freedRanges[i].offset;freedRanges[i]=freedRanges[--freedCount];break;}
    if(offset==~uint64_t{0}){if(arenaUsed+n*4096>ArenaBytes){pthread_mutex_unlock(&arenaLock);return VirtualAddress{};}offset=arenaUsed;arenaUsed+=n*4096;}
    pthread_mutex_unlock(&arenaLock);
    memset(arenaBase+offset,0,n*4096);
    return VirtualAddress(reinterpret_cast<uint64_t>(arenaBase+offset));
}
void VmmFreePages(VirtualAddress a,uint64_t n){
    pthread_mutex_lock(&arenaLock);
    const uint64_t offset=a.raw()-reinterpret_cast<uint64_t>(arenaBase);
    if(freedCount<4096)freedRanges[freedCount++]={offset,n};
    pthread_mutex_unlock(&arenaLock);
}
PhysicalAddress VmmVirtToPhys(PageTable,VirtualAddress a){
    const uint64_t base=reinterpret_cast<uint64_t>(arenaBase);
    if(!arenaBase || a.raw()<base || a.raw()-base>=ArenaBytes)return PhysicalAddress{};
    return PhysicalAddress(DmaBase+(a.raw()-base));
}
MappingBacking* MappingBackingCreate(void* object,void (*release)(void*)){
    auto* b=static_cast<MappingBacking*>(calloc(1,sizeof(MappingBacking)));if(!b)return nullptr;
    b->references=1;b->object=object;b->release=release;return b;
}
void MappingBackingRef(MappingBacking* b){__atomic_add_fetch(&b->references,1,__ATOMIC_ACQ_REL);}
void MappingBackingUnref(MappingBacking* b){if(!__atomic_sub_fetch(&b->references,1,__ATOMIC_ACQ_REL)){b->release(b->object);free(b);}}
void KMutexInit(KMutex* m){memset(m,0,sizeof(*m));}
void KMutexLock(KMutex* m){uint32_t e=0;while(!__atomic_compare_exchange_n(&m->locked,&e,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){e=0;sched_yield();}}
void KMutexUnlock(KMutex* m){__atomic_store_n(&m->locked,0,__ATOMIC_RELEASE);}
bool PanicRecoveryResetArmed(){return true;}
uint32_t SmpCurrentCpuIndex(){return 0;}
MemTag PmmGetTag(PhysicalAddress){return MemTag::Device;}
uint16_t PmmGetPid(PhysicalAddress){return KernelPid;}
const DellGpuVtdObservation& DellGpuVtdObserveRead(){static DellGpuVtdObservation o{};o.sampled=o.topology=o.inventory=o.bootMap=true;return o;}
bool VmmDellGpuVtdCriticalRead(DellGpuVtdStatus& s){s={};s.version=0x10;s.capability=32;s.protectedMemorySampled=true;return true;}
}
namespace std {[[noreturn]] void terminate() noexcept {abort();}}

// ---- interposition ----
namespace {
template<class F> F Real(const char* name){static_assert(sizeof(F)==sizeof(void*));void* p=dlsym(RTLD_NEXT,name);F f;memcpy(&f,&p,sizeof(f));return f;}
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
constexpr int MaxFd=4096;
BdwGpuFile* files[MaxFd]{};
FILE* logFile=nullptr;bool logInit=false;
struct Stats {uint64_t ioctls,execs,execBsd,execRender,execRefused,objects,maxObjects,maxPages,maxTables,refusedRelocs;uint64_t byNr[256],failByNr[256];} stats{};
const char* NodePath(){const char* p=getenv("BROOK_SHIM_NODE");return p && *p?p:"/dev/dri/renderD128";}
void Log(const char* fmt,...){
    if(!logInit){logInit=true;const char* p=getenv("BROOK_SHIM_LOG");if(p && *p)logFile=fopen(p,"we");}
    if(!logFile)return;va_list a;va_start(a,fmt);vfprintf(logFile,fmt,a);va_end(a);fflush(logFile);
}
bool UserRead(void*,void* dst,uint64_t src,uint64_t n){if(!src && n)return false;memcpy(dst,reinterpret_cast<const void*>(src),n);return true;}
bool UserWritable(void*,uint64_t dst,uint64_t){return dst!=0;}
bool UserWrite(void*,uint64_t dst,const void* src,uint64_t n){if(!dst && n)return false;memcpy(reinterpret_cast<void*>(dst),src,n);return true;}
uint64_t UserNow(void*){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec);}
bool UserSleep(void*,uint64_t ns){timespec t{time_t(ns/1000000000ull),long(ns%1000000000ull)};nanosleep(&t,nullptr);return true;}
// Map `length` bytes of the buffer behind `token` at a fresh address,
// page by page from the arena so CPU and "kernel" views share pages.
void* MapToken(BdwGpuFile* file,uint64_t token,uint64_t length,int prot){
    uint64_t relative=0;auto* buffer=BdwGpuFileGetMapping(file,token,length,relative);
    if(!buffer){errno=EINVAL;return MAP_FAILED;}
    length=(length+4095)&~uint64_t{4095};
    static auto realMmap=Real<void*(*)(void*,size_t,int,int,int,off_t)>("mmap");
    void* region=realMmap(nullptr,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(region==MAP_FAILED){BdwGpuBufferUnref(buffer);return MAP_FAILED;}
    for(uint64_t i=0;i<length/4096;++i){
        const uint64_t kva=BdwGpuBufferPageKernelAddress(buffer,uint32_t(relative/4096+i));
        const uint64_t offset=kva-reinterpret_cast<uint64_t>(arenaBase);
        if(!kva || realMmap(static_cast<uint8_t*>(region)+i*4096,4096,prot,MAP_SHARED|MAP_FIXED,arenaFd,off_t(offset))==MAP_FAILED){
            munmap(region,length);BdwGpuBufferUnref(buffer);errno=EINVAL;return MAP_FAILED;}
    }
    BdwGpuBufferUnref(buffer);return region;
}
uint64_t mapFdContext=0;
int64_t ShimMapBuffer(void* context,uint64_t token,uint64_t bytes,uint64_t& address){
    const int fd=int(*static_cast<uint64_t*>(context));
    if(fd<0 || fd>=MaxFd || !files[fd])return -EBADF;
    void* p=MapToken(files[fd],token,bytes,PROT_READ|PROT_WRITE);
    if(p==MAP_FAILED)return -errno;
    address=reinterpret_cast<uint64_t>(p);return 0;
}
// "Execution": validate the admitted snapshot's shape, record it, retire it
// as completed and signal its fences.
int64_t ShimSubmit(void*,BdwExecSnapshot* s){
    const uint32_t engine=BdwExecSnapshotEngine(s),objects=BdwExecSnapshotObjectCount(s),pages=BdwExecSnapshotPageCount(s);
    uint32_t tables=0;(void)BdwExecSnapshotTableCount(s,tables);
    ++stats.execs;if(engine==BdwExecEngineVideo)++stats.execBsd;else ++stats.execRender;
    stats.objects+=objects;if(objects>stats.maxObjects)stats.maxObjects=objects;
    if(pages>stats.maxPages)stats.maxPages=pages;if(tables>stats.maxTables)stats.maxTables=tables;
    const auto sample=BdwExecSnapshotBatchSample(s);
    Log("exec engine=%s objects=%u pages=%u tables=%u batch=%#lx len=%lu words=%08x %08x %08x %08x\n",
        engine==BdwExecEngineVideo?"bsd":"render",objects,pages,tables,BdwExecSnapshotBatchAddress(s),BdwExecSnapshotBatchLength(s),
        sample.words[0],sample.words[1],sample.words[2],sample.words[3]);
    if(!BdwExecSnapshotReserve(s))return -16;
    if(!BdwExecSnapshotRetire(s,true) || !BdwExecSnapshotSignalFences(s,true) || !BdwExecSnapshotRelease(s,true))abort();
    return 0;
}
int64_t Dispatch(int fd,unsigned long request,void* arg){
    BdwI915UserAccess user{nullptr,UserRead,UserWritable,UserWrite,UserNow,UserSleep};
    uint64_t context=uint64_t(fd);user.context=&context;
    user.submit=ShimSubmit;user.renderExperiment=true;user.mapBuffer=ShimMapBuffer;
    user.submitterPid=uint32_t(getpid());
    const uint64_t cmd=request;
    const int64_t r=BdwIntelRenderIoctl(files[fd],cmd,reinterpret_cast<uint64_t>(arg),user);
    const unsigned nr=unsigned(cmd&255);++stats.ioctls;++stats.byNr[nr];
    if(r<0){++stats.failByNr[nr];if(nr==0x69)++stats.execRefused;}
    uint64_t detail=0;
    if(nr==0x46 && arg){detail=uint32_t(*static_cast<int32_t*>(arg));}
    else if(nr==0x69 && arg){detail=static_cast<uint64_t*>(arg)[5];}
    if(nr==0x69 && r<0 && arg){
        // Refused execbuffer: dump the request so the cause is visible.
        struct Eb {uint64_t objects;uint32_t count,start,length,dr1,dr4,clips;uint64_t clipPtr,flags,context,fences;};
        struct Obj {uint32_t handle,relocs;uint64_t relocPtr,alignment,offset,flags,r1,r2;};
        const auto* eb=static_cast<const Eb*>(arg);const auto* o=reinterpret_cast<const Obj*>(eb->objects);
        Log("refused-exec count=%u start=%u len=%u flags=%#lx ctx=%lu clips=%u fences=%#lx\n",eb->count,eb->start,eb->length,eb->flags,
            eb->context,eb->clips,eb->fences);
        for(uint32_t i=0;o && i<eb->count && i<64;++i)
            Log("  obj[%u] handle=%u relocs=%u align=%#lx offset=%#lx flags=%#lx r1=%#lx r2=%#lx\n",i,o[i].handle,o[i].relocs,
                o[i].alignment,o[i].offset,o[i].flags,o[i].r1,o[i].r2);
    }
    Log("ioctl %-28s nr=%#04x size=%u detail=%#lx -> %ld%s%s\n",BdwIoctlName(nr),nr,unsigned((cmd>>16)&0x3fff),detail,long(r),
        r<0 && nr==0x69?" reason=":"",r<0 && nr==0x69?(BdwTakeRefusal()?:"?"):"");
    return r;
}
bool Ours(int fd){return fd>=0 && fd<MaxFd && files[fd];}
void Track(int fd){
    if(fd<0 || fd>=MaxFd)return;
    if(!getenv("BROOK_SHIM_BSD") || strcmp(getenv("BROOK_SHIM_BSD"),"0"))__atomic_store_n(&BdwVideoUserSubmissionStorage(),1u,__ATOMIC_RELEASE);
    pthread_mutex_lock(&lock);ArenaInit();
    if(!files[fd])files[fd]=BdwGpuFileCreate();
    pthread_mutex_unlock(&lock);
    Log("open node fd=%d\n",fd);
}
void TrackDup(int from,int to){
    if(!Ours(from) || to<0 || to>=MaxFd)return;
    pthread_mutex_lock(&lock);BdwGpuFileRef(files[from]);files[to]=files[from];pthread_mutex_unlock(&lock);
}
bool IsNode(const char* path){return path && !strcmp(path,NodePath());}
__attribute__((destructor)) void Summary(){
    if(!stats.ioctls)return;
    fprintf(stderr,"brook-i915-shim: ioctls=%lu execs=%lu (bsd=%lu render=%lu) refused=%lu max_objects=%lu max_pages=%lu max_tables=%lu\n",
        stats.ioctls,stats.execs,stats.execBsd,stats.execRender,stats.execRefused,stats.maxObjects,stats.maxPages,stats.maxTables);
    for(unsigned i=0;i<256;++i)if(stats.failByNr[i])fprintf(stderr,"brook-i915-shim: failed %s x%lu of %lu\n",BdwIoctlName(i),stats.failByNr[i],stats.byNr[i]);
}
}
extern "C" {
int open(const char* path,int flags,...){
    static auto real=Real<int(*)(const char*,int,...)>("open");
    mode_t mode=0;if(flags&(O_CREAT|O_TMPFILE)){va_list a;va_start(a,flags);mode=mode_t(va_arg(a,int));va_end(a);}
    const int fd=real(path,flags,mode);if(fd>=0 && IsNode(path))Track(fd);return fd;
}
int open64(const char* path,int flags,...){
    mode_t mode=0;if(flags&(O_CREAT|O_TMPFILE)){va_list a;va_start(a,flags);mode=mode_t(va_arg(a,int));va_end(a);}
    return open(path,flags,mode);
}
int openat(int dir,const char* path,int flags,...){
    static auto real=Real<int(*)(int,const char*,int,...)>("openat");
    mode_t mode=0;if(flags&(O_CREAT|O_TMPFILE)){va_list a;va_start(a,flags);mode=mode_t(va_arg(a,int));va_end(a);}
    const int fd=real(dir,path,flags,mode);if(fd>=0 && IsNode(path))Track(fd);return fd;
}
int openat64(int dir,const char* path,int flags,...){
    mode_t mode=0;if(flags&(O_CREAT|O_TMPFILE)){va_list a;va_start(a,flags);mode=mode_t(va_arg(a,int));va_end(a);}
    return openat(dir,path,flags,mode);
}
int close(int fd){
    static auto real=Real<int(*)(int)>("close");
    if(Ours(fd)){pthread_mutex_lock(&lock);BdwGpuFileUnref(files[fd]);files[fd]=nullptr;pthread_mutex_unlock(&lock);}
    return real(fd);
}
int dup(int fd){static auto real=Real<int(*)(int)>("dup");const int r=real(fd);if(r>=0)TrackDup(fd,r);return r;}
int dup2(int fd,int to){static auto real=Real<int(*)(int,int)>("dup2");if(Ours(to) && to!=fd)close(to);const int r=real(fd,to);if(r>=0 && r!=fd)TrackDup(fd,r);return r;}
int dup3(int fd,int to,int flags){static auto real=Real<int(*)(int,int,int)>("dup3");if(Ours(to))close(to);const int r=real(fd,to,flags);if(r>=0)TrackDup(fd,r);return r;}
int fcntl(int fd,int cmd,...){
    static auto real=Real<int(*)(int,int,...)>("fcntl");
    va_list a;va_start(a,cmd);long v=va_arg(a,long);va_end(a);
    const int r=real(fd,cmd,v);
    if(r>=0 && (cmd==F_DUPFD || cmd==F_DUPFD_CLOEXEC))TrackDup(fd,r);
    return r;
}
int ioctl(int fd,unsigned long request,...){
    static auto real=Real<int(*)(int,unsigned long,...)>("ioctl");
    va_list a;va_start(a,request);void* arg=va_arg(a,void*);va_end(a);
    if(!Ours(fd))return real(fd,request,arg);
    const int64_t r=Dispatch(fd,request,arg);
    if(r<0){errno=int(-r);return -1;}
    return int(r);
}
void* mmap(void* addr,size_t length,int prot,int flags,int fd,off_t offset){
    static auto real=Real<void*(*)(void*,size_t,int,int,int,off_t)>("mmap");
    if(!Ours(fd))return real(addr,length,prot,flags,fd,offset);
    if(!(flags&MAP_SHARED) || addr){errno=EINVAL;return MAP_FAILED;}
    void* p=MapToken(files[fd],uint64_t(offset),length,prot);
    Log("mmap fd=%d token=%#lx len=%zu -> %p\n",fd,uint64_t(offset),length,p);
    return p;
}
void* mmap64(void* addr,size_t length,int prot,int flags,int fd,off_t offset){return mmap(addr,length,prot,flags,fd,offset);}
}
