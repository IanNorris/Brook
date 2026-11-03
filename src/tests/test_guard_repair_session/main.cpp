#include <cassert>
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
#include "guard_repair_session.h"
#include "device.h"
#include "process.h"
#include "vfs.h"
using namespace brook;
namespace {
Device* selected=nullptr;thread_local Process* current=nullptr;
uint8_t storage[4096]{};int flushError=0,writeError=0;unsigned flushes=0,writes=0,live=0;
std::mutex ioMutex;std::condition_variable ioCv;bool blockWrite=false,writeEntered=false,writeReleased=false;
int read(Device*,uint64_t at,void* out,uint64_t n) {assert(at<=sizeof(storage) && n<=sizeof(storage)-at);std::memcpy(out,storage+at,n);return int(n);}
int write(Device*,uint64_t at,const void* in,uint64_t n) {
    {std::unique_lock<std::mutex> guard(ioMutex);if(blockWrite) {writeEntered=true;ioCv.notify_all();ioCv.wait(guard,[]{return writeReleased;});}}
    ++writes;if(writeError)return writeError;assert(at<=sizeof(storage) && n<=sizeof(storage)-at);std::memcpy(storage+at,in,n);return int(n);
}
int ioctl(Device*,uint32_t op,void*) {
    if(op==DEVICE_IOCTL_FLUSH) {
        {std::unique_lock<std::mutex> guard(ioMutex);if(blockWrite) {writeEntered=true;ioCv.notify_all();ioCv.wait(guard,[]{return writeReleased;});}}
        ++flushes;return flushError;
    }
    return -95;
}
uint64_t blocks(Device*) {return 8;}uint32_t blockSize(Device*) {return 512;}
}
namespace brook {
Process* ProcessCurrent(){return current;}
CredentialIds ProcessReadIds(Process* p,bool){return {p->uid,p->euid,p->suid};}
bool AhciSystemGuardIsTestData(Device* p){return p==selected;}
extern "C" bool DeviceIsRegistered(Device* p){return p==selected;}
extern "C" void* kmalloc(uint64_t n){void* p=std::malloc(n);if(p)++live;return p;}
extern "C" void kfree(void* p){if(p)--live;std::free(p);}
extern "C" int DeviceWrite(Device* d,uint64_t at,const void* p,uint64_t n){return d->ops->write(d,at,p,n);}
extern "C" int DeviceFlush(Device* d){return d->ops->ioctl(d,DEVICE_IOCTL_FLUSH,nullptr);}
void VfsRelease(Vnode* n){if(n && __atomic_sub_fetch(&n->refCount,1,__ATOMIC_ACQ_REL)==0){n->ops->close(n);kfree(n);}}
}
int main() {
    BlockDeviceOps ops{read,write,ioctl,nullptr,blocks,blockSize};
    Device dev{reinterpret_cast<const DeviceOps*>(&ops),"test",DeviceType::Block,nullptr};selected=&dev;
    Process owner{},other{};owner.pid=owner.tgid=5;owner.incarnation=42;other.pid=other.tgid=6;other.incarnation=43;
    current=&owner;int error=0;assert(!GuardRepairOpenLinux(2,&error));
    for(unsigned mode=0;mode<2;++mode) {
        const pid_t busy=fork();assert(busy>=0);
        if(!busy) {
            assert(GuardRepairBegin(&dev,&owner,true));Vnode* n=GuardRepairOpenLinux(2,&error);assert(n);
            blockWrite=true;int result=0;
            std::thread worker([&]{current=&owner;uint64_t at=0;uint8_t byte=7;
                result=mode?n->ops->fsync(n):n->ops->write(n,&byte,1,&at);});
            {std::unique_lock<std::mutex> guard(ioMutex);ioCv.wait(guard,[]{return writeEntered;});}
            GuardRepairAbort();assert(!GuardRepairQuiescent());assert(!GuardRepairOpenLinux(0,&error));
            {std::lock_guard<std::mutex> guard(ioMutex);writeReleased=true;}ioCv.notify_all();worker.join();
            assert(result==(mode?0:1));assert(!GuardRepairQuiescent());VfsRelease(n);
            assert(GuardRepairQuiescent() && !GuardRepairBegin(&dev,&owner,false));_exit(0);
        }
        int status=0;assert(waitpid(busy,&status,0)==busy && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    assert(GuardRepairBegin(&dev,&owner,true));
    current=&other;assert(!GuardRepairOpenLinux(2,&error));current=&owner;
    assert(!GuardRepairOpenLinux(1,&error) && error==-30);
    assert(!GuardRepairOpenLinux(0x200002,&error) && error==-30);
    Vnode* rw=GuardRepairOpenLinux(2|0x80,&error);assert(rw && !error && rw->ops->deny_mapping);
    assert(!GuardRepairOpenLinux(2,&error));
    Vnode* ro=GuardRepairOpenLinux(0,&error);assert(ro && !error);
    uint8_t data[4]={1,2,3,4},out[4]{};uint64_t at=4094;
    assert(rw->ops->write(rw,data,4,&at)==2 && at==4096 && writes==1);
    at=4094;assert(ro->ops->read(ro,out,4,&at)==2 && out[0]==1 && out[1]==2);
    assert(ro->ops->write(ro,data,1,&at)==-30);assert(rw->ops->truncate(rw,1)==-30);
    at=4096;assert(rw->ops->write(rw,data,1,&at)==-28);
    assert(!rw->ops->fsync(rw) && flushes==1);flushError=-5;assert(rw->ops->fsync(rw)==-5);flushError=0;
    current=&other;at=0;assert(rw->ops->read(rw,out,1,&at)==-13);current=&owner;
    VfsRelease(rw);VfsRelease(ro);assert(!live && GuardRepairEnd());
    Process verifier{};verifier.pid=verifier.tgid=7;verifier.incarnation=55;current=&verifier;
    assert(GuardRepairBegin(&dev,&verifier,false));assert(!GuardRepairOpenLinux(2,&error));
    ro=GuardRepairOpenLinux(0,&error);assert(ro && ro->ops->write(ro,data,1,&at)==-30);
    VfsRelease(ro);assert(GuardRepairEnd() && !live);
    assert(!GuardRepairBegin(&dev,&verifier,false));
}
