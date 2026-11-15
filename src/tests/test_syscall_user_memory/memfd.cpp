// H-24 / H-11 / H-16: user memory is never touched under a spinlock (memfd
// MfdLock, pipe->lock behind /dev/tty and AF_UNIX streams).
#include "harness.h"
#include "syscall_memfd.cpp"
#include "syscall_unix_socket.h"
#include "syscall_handlers.h"
#include <stdlib.h>

using namespace harness;
namespace {
brook::MemFdData* g_mfd;
unsigned g_copiesUnderLock,g_selfFaults;
brook::PipeBuffer* g_watchedPipe;
brook::MemFdData* NewMemfd(uint64_t pages) {
    auto* mfd=static_cast<brook::MemFdData*>(calloc(1,sizeof(brook::MemFdData)));
    mfd->refCount=1;
    CHECK(brook::MemFdGrowLocked(mfd,pages*4096));
    return mfd;
}
// A copy into the memfd's own unfaulted MAP_SHARED mapping faults into
// MemFdGetOrAllocPage, which takes MfdLock: it must not already be held.
void MemfdCopyHook(uint64_t,uint64_t,bool) {
    if(g_mfd->lock) { ++g_copiesUnderLock;return; }
    if(brook::MemFdGetOrAllocPage(g_mfd,3,7))++g_selfFaults;
}
void PipeCopyHook(uint64_t,uint64_t,bool) { if(SpinLockHeld(g_watchedPipe->lock))++g_copiesUnderLock; }
}
namespace brook {
void PmmFreePage(PhysicalAddress) {}
PhysicalAddress PmmAllocPage(MemTag,uint16_t) {
    void* page=aligned_alloc(4096,4096);
    return PhysicalAddress(reinterpret_cast<uint64_t>(page)-DIRECT_MAP_BASE);
}
}

static void MemfdUnlockedCopies() {
    Reset();ResetProcess();
    g_mfd=NewMemfd(4);g_copiesUnderLock=g_selfFaults=0;
    g_userCopyHook=MemfdCopyHook;
    // Write 6000 bytes straddling two pages from user memory.
    auto* source=reinterpret_cast<uint8_t*>(UserAddress(0));
    for(int i=0;i<6000;++i)source[i]=static_cast<uint8_t>(i*7);
    uint64_t offset=100;
    CHECK(brook::MemFdWriteAt(g_mfd,UserAddress(0),6000,&offset,false,true)==6000);
    CHECK(offset==6100 && g_mfd->size==6100);
    // Read back into user memory.
    offset=100;
    CHECK(brook::MemFdReadAt(g_mfd,UserAddress(2*PageSize),6000,&offset,true)==6000);
    CHECK(__builtin_memcmp(source,reinterpret_cast<const void*>(UserAddress(2*PageSize)),6000)==0);
    CHECK(g_userCopies>=4);
    CHECK(g_copiesUnderLock==0);
    CHECK(g_selfFaults==g_userCopies);
    // A destination that faults part-way returns the bytes before the fault.
    g_userCopyHook=nullptr;
    Protect(5*PageSize,PageSize,Access::None);
    offset=0;
    CHECK(brook::MemFdReadAt(g_mfd,UserAddress(5*PageSize-4096),6000,&offset,true)==4096);
    CHECK(offset==4096);
    offset=0;
    CHECK(brook::MemFdReadAt(g_mfd,UserAddress(5*PageSize),16,&offset,true)==-brook::EFAULT);
    CHECK(brook::MemFdWriteAt(g_mfd,UserAddress(5*PageSize),16,&offset,false,true)==-brook::EFAULT);
    // Kernel buffers (sendfile's bounce) are copied in place.
    uint8_t kernel[32];offset=100;
    CHECK(brook::MemFdReadAt(g_mfd,reinterpret_cast<uint64_t>(kernel),32,&offset,false)==32);
    CHECK(kernel[5]==static_cast<uint8_t>(5*7));
    // sys_read on the descriptor uses the user path.
    auto* proc=CurrentProcess();
    proc->fds[3].type=brook::FdType::MemFd;proc->fds[3].handle=g_mfd;Describe(proc->fds[3],2);
    g_userCopies=0;g_userCopyHook=MemfdCopyHook;
    CHECK(brook::sys_read(3,UserAddress(8*PageSize),64,0,0,0)==64);
    CHECK(g_userCopies>0 && g_copiesUnderLock==0);
    g_userCopyHook=nullptr;
}

static void TtyAndUnixStreamUnlockedCopies() {
    Reset();ResetProcess();
    auto* proc=CurrentProcess();
    // /dev/tty in terminal mode reads the stdin pipe and writes the stdout pipe.
    auto* in=brook::PipeBufferCreate(4096);auto* out=brook::PipeBufferCreate(4096);
    in->readers=in->writers=1;out->readers=out->writers=1;
    brook::TtyDevicePair pair{in,out};
    proc->fds[4].type=brook::FdType::DevTty;proc->fds[4].handle=&pair;Describe(proc->fds[4],2);
    CHECK(in->write("hello",5)==5);
    g_copiesUnderLock=0;g_watchedPipe=in;g_userCopies=0;g_userCopyHook=PipeCopyHook;
    CHECK(brook::sys_read(4,UserAddress(0),16,0,0,0)==5);
    CHECK(g_userCopies==1 && g_copiesUnderLock==0);
    CHECK(__builtin_memcmp(reinterpret_cast<const void*>(UserAddress(0)),"hello",5)==0);
    // A kernel destination never reaches the pipe copy.
    CHECK(in->write("x",1)==1);
    CHECK(brook::sys_read(4,g_kernel.Address(),16,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    // AF_UNIX stream: stage both directions outside pipe->lock.
    int64_t vector[2]={-1,-1};(void)vector;
    auto* a=static_cast<brook::UnixSocketData*>(calloc(1,sizeof(brook::UnixSocketData)));
    auto* b=static_cast<brook::UnixSocketData*>(calloc(1,sizeof(brook::UnixSocketData)));
    auto* ab=brook::PipeBufferCreate(65536);ab->readers=ab->writers=1;
    a->state=b->state=brook::UnixSocketData::State::Connected;a->socketType=b->socketType=1;
    a->txPipe=ab;b->rxPipe=ab;a->refCount=b->refCount=1;
    __builtin_memcpy(reinterpret_cast<void*>(UserAddress(PageSize)),"stream-bytes",12);
    g_copiesUnderLock=0;g_watchedPipe=ab;g_userCopies=0;
    CHECK(brook::UnixSocketWriteSome(a,true,reinterpret_cast<const char*>(UserAddress(PageSize)),12)==12);
    brook::UnixFdSnap descriptors[brook::UNIX_FD_QUEUE_CAP];int received=0;
    CHECK(brook::UnixSocketReadSome(b,true,reinterpret_cast<char*>(UserAddress(2*PageSize)),64,descriptors,received)==12);
    CHECK(g_userCopies==2 && g_copiesUnderLock==0);
    CHECK(__builtin_memcmp(reinterpret_cast<const void*>(UserAddress(2*PageSize)),"stream-bytes",12)==0);
    // A faulting source is EFAULT with nothing queued.
    Protect(3*PageSize,PageSize,Access::None);
    CHECK(brook::UnixSocketWriteSome(a,true,reinterpret_cast<const char*>(UserAddress(3*PageSize)),8)==-brook::EFAULT);
    CHECK(ab->empty());
    g_userCopyHook=nullptr;
}

void RunMemfdTests() {
    MemfdUnlockedCopies();
    TtyAndUnixStreamUnlockedCopies();
}
