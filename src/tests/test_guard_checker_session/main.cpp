#include <cassert>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#undef SA_SIGINFO
#undef SA_ONSTACK
#undef SA_RESTORER
#undef SA_RESTART
#undef SA_NODEFER
#include "guard_checker_session.h"
#include "device.h"
#include "process.h"
#include "vfs.h"

using namespace brook;
namespace {
Device* reserved=nullptr;thread_local Process* current=nullptr;
std::atomic<int> live{0};int failAlloc=-1;
std::mutex allocationMutex,ioMutex,beginMutex;std::condition_variable allocationCv,ioCv,beginCv;
bool blockAllocation=false,allocationEntered=false,allocationReleased=false;
bool blockRead=false,readEntered=false,readReleased=false;
bool blockBegin=false,beginEntered=false,beginReleased=false;
unsigned writes=0,flushes=0;int readError=0;
int Read(Device*,uint64_t,void* out,uint64_t size) {
    std::unique_lock<std::mutex> guard(ioMutex);
    if(blockRead) {readEntered=true;ioCv.notify_all();ioCv.wait(guard,[]{return readReleased;});}
    if(readError)return readError;
    std::memset(out,0x39,size);return int(size);
}
int Write(Device*,uint64_t,const void*,uint64_t) {++writes;return -5;}
int Ioctl(Device*,uint32_t op,void*) {
    if(op==DEVICE_IOCTL_READ_ONLY) {
        std::unique_lock<std::mutex> guard(beginMutex);
        if(blockBegin) {beginEntered=true;beginCv.notify_all();beginCv.wait(guard,[]{return beginReleased;});}
        return 1;
    }
    if(op==DEVICE_IOCTL_FLUSH)++flushes;
    return -95;
}
uint64_t Blocks(Device*) {return 8;}uint32_t BlockSize(Device*) {return 512;}
}
namespace brook {
Process* ProcessCurrent(){return current;}
CredentialIds ProcessReadIds(Process* p,bool){return {p->uid,p->euid,p->suid};}
bool AhciSystemGuardIsReservedDevice(Device* p){return p==reserved;}
extern "C" bool DeviceIsRegistered(Device* p){return p==reserved;}
extern "C" void* kmalloc(uint64_t n){
    {std::unique_lock<std::mutex> guard(allocationMutex);
     if(blockAllocation) {allocationEntered=true;allocationCv.notify_all();allocationCv.wait(guard,[]{return allocationReleased;});}}
    if(failAlloc==0)return nullptr;
    if(failAlloc>0)--failAlloc;
    void* p=std::malloc(n);if(p)++live;return p;
}
extern "C" void kfree(void* p){if(p)--live;std::free(p);}
void VfsRelease(Vnode* node){if(node && __atomic_sub_fetch(&node->refCount,1,__ATOMIC_ACQ_REL)==0){node->ops->close(node);kfree(node);}}
}
static Process process(uint16_t pid,uint16_t tgid,uint64_t incarnation,uint64_t leader) {
    Process p{};p.pid=pid;p.tgid=tgid;p.incarnation=incarnation;p.leaderIncarnation=leader;return p;
}
static void childDone(pid_t pid) {int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));}
int main() {
    BlockDeviceOps block{Read,Write,Ioctl,nullptr,Blocks,BlockSize};
    Device device{reinterpret_cast<const DeviceOps*>(&block),"guard",DeviceType::Block,nullptr};reserved=&device;
    Process owner=process(5,5,77,0),sibling=process(6,5,78,77),other=process(8,8,88,0),reused=process(5,5,99,0);
    int error=0;current=&owner;assert(!GuardCheckerOpen(VFS_O_READ,&error));
    const pid_t abortFirst=fork();assert(abortFirst>=0);
    if(!abortFirst) {GuardCheckerAbort();assert(!GuardCheckerBegin(&device,&owner));_exit(0);}
    childDone(abortFirst);
    const pid_t abortDuringBegin=fork();assert(abortDuringBegin>=0);
    if(!abortDuringBegin) {
        blockBegin=true;bool result=true;
        std::thread starter([&]{result=GuardCheckerBegin(&device,&owner);});
        {std::unique_lock<std::mutex> g(beginMutex);beginCv.wait(g,[]{return beginEntered;});}
        GuardCheckerAbort();
        {std::lock_guard<std::mutex> g(beginMutex);beginReleased=true;}beginCv.notify_all();starter.join();
        assert(!result && !GuardCheckerGetStatus().active);_exit(0);
    }
    childDone(abortDuringBegin);
    const pid_t beginRace=fork();assert(beginRace>=0);
    if(!beginRace) {
        std::atomic<unsigned> readyCount{0};std::atomic<bool> go{false};bool first=false,second=false;
        auto begin=[&](bool& result){++readyCount;while(!go.load())std::this_thread::yield();result=GuardCheckerBegin(&device,&owner);};
        std::thread a(begin,std::ref(first)),b(begin,std::ref(second));
        while(readyCount.load()!=2)std::this_thread::yield();
        go=true;a.join();b.join();assert(first!=second);GuardCheckerAbort();
        assert(!GuardCheckerBegin(&device,&owner));_exit(0);
    }
    childDone(beginRace);
    const pid_t allocation=fork();assert(allocation>=0);
    if(!allocation) {
        assert(GuardCheckerBegin(&device,&owner));blockAllocation=true;Vnode* result=nullptr;
        std::thread opener([&]{current=&owner;result=GuardCheckerOpen(VFS_O_READ,&error);});
        {std::unique_lock<std::mutex> g(allocationMutex);allocationCv.wait(g,[]{return allocationEntered;});}
        assert(GuardCheckerGetStatus().views==1);GuardCheckerAbort();
        {std::lock_guard<std::mutex> g(allocationMutex);allocationReleased=true;}allocationCv.notify_all();opener.join();
        assert(!result && error==-13 && !GuardCheckerGetStatus().views && !live);_exit(0);
    }
    childDone(allocation);
    const pid_t reading=fork();assert(reading>=0);
    if(!reading) {
        assert(GuardCheckerBegin(&device,&owner));Vnode* node=GuardCheckerOpen(VFS_O_READ,&error);assert(node);
        blockRead=true;int result=0;uint8_t byte=0;uint64_t at=0;
        VfsRetain(node);
        std::thread reader([&]{current=&owner;result=node->ops->read(node,&byte,1,&at);VfsRelease(node);});
        {std::unique_lock<std::mutex> g(ioMutex);ioCv.wait(g,[]{return readEntered;});}
        GuardCheckerAbort();auto status=GuardCheckerGetStatus();assert(!status.active && status.views==1 && status.reads==1);
        assert(!GuardCheckerOpen(VFS_O_READ,&error));
        current=&other;VfsRelease(node);assert(GuardCheckerGetStatus().views==1);
        {std::lock_guard<std::mutex> g(ioMutex);readReleased=true;}ioCv.notify_all();reader.join();
        assert(result==1 && byte==0x39 && !GuardCheckerGetStatus().reads && !GuardCheckerGetStatus().views && !live);_exit(0);
    }
    childDone(reading);
    owner.euid=1000;assert(!GuardCheckerBegin(&device,&owner));owner.euid=0;
    assert(GuardCheckerBegin(&device,&owner));assert(!GuardCheckerBegin(&device,&owner));
    for(int i=0;i<4;++i){failAlloc=i;assert(!GuardCheckerOpen(VFS_O_READ,&error) && error==-12);assert(!GuardCheckerGetStatus().views && !live);}
    failAlloc=-1;
    current=&other;assert(!GuardCheckerOpen(VFS_O_READ,&error));current=&reused;assert(!GuardCheckerOpen(VFS_O_READ,&error));
    current=&sibling;Vnode* node=GuardCheckerOpen(VFS_O_READ,&error);assert(node && !error);
    assert(node->ops->deny_mapping && !node->ops->file_object && !node->cacheId);
    uint8_t byte=0;uint64_t at=0;current=&other;assert(node->ops->read(node,&byte,1,&at)==-13);
    assert(node->ops->open(node,VFS_O_READ)==-13 && node->ops->access(node,4,false)==-13);
    current=&reused;assert(node->ops->read(node,&byte,1,&at)==-13);
    current=&owner;assert(node->ops->read(node,&byte,1,&at)==1 && byte==0x39);
    readError=-5;assert(node->ops->read(node,&byte,1,&at)==-5 && at==1 && !GuardCheckerGetStatus().reads);readError=0;
    VfsRetain(node);VfsRelease(node);assert(GuardCheckerGetStatus().views==1);
    GuardCheckerAbort();assert(node->ops->read(node,&byte,1,&at)==-13);
    assert(node->ops->open(node,VFS_O_READ)==-13 && node->ops->access(node,4,false)==-13);
    current=&other;VfsRelease(node);auto status=GuardCheckerGetStatus();
    assert(!status.active && !status.views && !status.reads && !live && !writes && !flushes);
    assert(!GuardCheckerBegin(&device,&owner));
}
