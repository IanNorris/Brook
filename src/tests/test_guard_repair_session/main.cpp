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
#include "scheduler.h"
#include "vfs.h"
using namespace brook;
namespace {
Device* selected=nullptr;thread_local Process* current=nullptr;
uint8_t storage[4096]{};int flushError=0,writeError=0;unsigned flushes=0,writes=0,live=0;
std::mutex ioMutex;std::condition_variable ioCv;bool blockWrite=false,writeEntered=false,writeReleased=false;
std::mutex waitMutex;std::condition_variable waitCv;bool blockWait=false,waitEntered=false,waitReleased=false;
int fakeWaitOutcome=1;ChildWaitSnapshot fakeEvent{};
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
int SchedulerWaitChild(Process*,int,uint32_t,uint32_t,ChildWaitSnapshot* out) {
    std::unique_lock<std::mutex> guard(waitMutex);
    if(blockWait){waitEntered=true;waitCv.notify_all();waitCv.wait(guard,[]{return waitReleased;});}
    if(fakeWaitOutcome==1)*out=fakeEvent;return fakeWaitOutcome;
}
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
    assert(GuardRepairRejectedOutcome(true)==-5);
    assert(GuardRepairRejectedOutcome(false)==-115);
    BlockDeviceOps ops{read,write,ioctl,nullptr,blocks,blockSize};
    Device dev{reinterpret_cast<const DeviceOps*>(&ops),"test",DeviceType::Block,nullptr};selected=&dev;
    Process owner{},other{};owner.pid=owner.tgid=5;owner.incarnation=42;other.pid=other.tgid=6;other.incarnation=43;
    current=&owner;int error=0;assert(!GuardRepairOpenLinux(2,&error));
    for(int status: {0,256,512,1024}) {
        const pid_t test=fork();assert(test>=0);
        if(!test) {
            Process control{},replacement{};control.pid=control.tgid=10;control.incarnation=100;control.isKernelThread=true;
            replacement=control;replacement.incarnation=101;owner.parentPid=10;current=&control;
            ChildWaitSnapshot event{};fakeEvent.pid=5;fakeEvent.incarnation=42;fakeEvent.code=1;fakeEvent.status=status;
            assert(GuardRepairBeginTracked(&dev,&owner,true));
            assert(GuardRepairPollTracked(nullptr,5,42,&event)<0);
            assert(GuardRepairPollTracked(&dev,5,43,&event)<0);
            current=&replacement;assert(GuardRepairPollTracked(&dev,5,42,&event)<0);current=&control;
            fakeWaitOutcome=0;assert(GuardRepairPollTracked(&dev,5,42,&event)==0);
            fakeWaitOutcome=1;assert(GuardRepairPollTracked(&dev,5,42,&event)==1);
            if(status==0 || status==256) {
                assert(!GuardRepairConsumeCompletion(&dev,false));
                current=&replacement;assert(!GuardRepairConsumeCompletion(&dev,true));current=&control;
                assert(GuardRepairConsumeCompletion(&dev,true));assert(!GuardRepairConsumeCompletion(&dev,true));
                Process verifier{};verifier.pid=verifier.tgid=7;verifier.incarnation=55;verifier.parentPid=10;
                assert(GuardRepairBeginTracked(&dev,&verifier,false));
                fakeEvent.pid=7;fakeEvent.incarnation=55;fakeEvent.status=0;
                assert(GuardRepairPollTracked(&dev,7,55,&event)==1);
                assert(!GuardRepairConsumeCompletion(&dev,true));assert(GuardRepairConsumeCompletion(&dev,false));
                assert(!GuardRepairConsumeCompletion(&dev,false));
            } else assert(!GuardRepairConsumeCompletion(&dev,true));
            _exit(0);
        }
        int statusResult=0;assert(waitpid(test,&statusResult,0)==test && WIFEXITED(statusResult) && !WEXITSTATUS(statusResult));
    }
    for(int fault=0;fault<5;++fault) {
        const pid_t test=fork();assert(test>=0);
        if(!test) {
            Process control{};control.pid=control.tgid=10;control.incarnation=100;control.isKernelThread=true;owner.parentPid=10;current=&control;
            fakeEvent.pid=fault==4?6:5;fakeEvent.incarnation=fault==0?43:42;fakeEvent.code=fault==1?2:1;fakeEvent.status=0;
            ChildWaitSnapshot event{};assert(GuardRepairBeginTracked(&dev,&owner,true));Vnode* held=nullptr;
            if(fault==2){current=&owner;held=GuardRepairOpenLinux(2,&error);assert(held);current=&control;}
            if(fault==3)fakeWaitOutcome=-10;
            assert(GuardRepairPollTracked(&dev,5,42,&event)==(fault==3?-10:1));
            if(held)VfsRelease(held);assert(!GuardRepairConsumeCompletion(&dev,true));
            assert(!GuardRepairBeginTracked(&dev,&owner,false));_exit(0);
        }
        int statusResult=0;assert(waitpid(test,&statusResult,0)==test && WIFEXITED(statusResult) && !WEXITSTATUS(statusResult));
    }
    {
        const pid_t test=fork();assert(test>=0);
        if(!test) {
            Process control{};control.pid=control.tgid=10;control.incarnation=100;control.isKernelThread=true;owner.parentPid=10;current=&control;
            ChildWaitSnapshot event{};fakeEvent.pid=5;fakeEvent.incarnation=42;fakeEvent.code=1;fakeEvent.status=0;
            assert(GuardRepairBeginTracked(&dev,&owner,true));assert(GuardRepairPollTracked(&dev,5,42,&event)==1);
            assert(GuardRepairConsumeCompletion(&dev,true));
            Process verifier{};verifier.pid=verifier.tgid=7;verifier.incarnation=55;verifier.parentPid=10;
            assert(GuardRepairBeginTracked(&dev,&verifier,false));fakeEvent.pid=7;fakeEvent.incarnation=55;fakeEvent.status=256;
            assert(GuardRepairPollTracked(&dev,7,55,&event)==1);assert(!GuardRepairConsumeCompletion(&dev,false));_exit(0);
        }
        int statusResult=0;assert(waitpid(test,&statusResult,0)==test && WIFEXITED(statusResult) && !WEXITSTATUS(statusResult));
    }
    for(unsigned flush=0;flush<2;++flush) {
        const pid_t test=fork();assert(test>=0);
        if(!test) {
            Process control{};control.pid=control.tgid=10;control.incarnation=100;control.isKernelThread=true;owner.parentPid=10;current=&control;
            fakeEvent.pid=5;fakeEvent.incarnation=42;fakeEvent.code=1;fakeEvent.status=0;
            assert(GuardRepairBeginTracked(&dev,&owner,true));current=&owner;Vnode* node=GuardRepairOpenLinux(2,&error);assert(node);
            blockWrite=true;int ioResult=0;
            std::thread worker([&]{current=&owner;uint64_t at=0;uint8_t byte=1;
                ioResult=flush?node->ops->fsync(node):node->ops->write(node,&byte,1,&at);});
            {std::unique_lock<std::mutex> guard(ioMutex);ioCv.wait(guard,[]{return writeEntered;});}
            current=&control;ChildWaitSnapshot event{};assert(GuardRepairPollTracked(&dev,5,42,&event)==1);
            assert(!GuardRepairQuiescent() && !GuardRepairConsumeCompletion(&dev,true));
            {std::lock_guard<std::mutex> guard(ioMutex);writeReleased=true;}ioCv.notify_all();worker.join();
            assert(ioResult==(flush?0:1));VfsRelease(node);assert(GuardRepairQuiescent());
            assert(!GuardRepairConsumeCompletion(&dev,true));_exit(0);
        }
        int statusResult=0;assert(waitpid(test,&statusResult,0)==test && WIFEXITED(statusResult) && !WEXITSTATUS(statusResult));
    }
    {
        const pid_t test=fork();assert(test>=0);
        if(!test) {
            Process control{};control.pid=control.tgid=10;control.incarnation=100;control.isKernelThread=true;owner.parentPid=10;current=&control;
            assert(GuardRepairBeginTracked(&dev,&owner,true));blockWait=true;fakeWaitOutcome=0;int result=0;
            std::thread poller([&]{current=&control;ChildWaitSnapshot event{};result=GuardRepairPollTracked(&dev,5,42,&event);});
            {std::unique_lock<std::mutex> guard(waitMutex);waitCv.wait(guard,[]{return waitEntered;});}
            ChildWaitSnapshot event{};assert(GuardRepairPollTracked(&dev,5,42,&event)<0);GuardRepairAbort();assert(!GuardRepairQuiescent());
            {std::lock_guard<std::mutex> guard(waitMutex);waitReleased=true;}waitCv.notify_all();poller.join();
            assert(result==-125 && GuardRepairQuiescent() && !GuardRepairConsumeCompletion(&dev,true));_exit(0);
        }
        int statusResult=0;assert(waitpid(test,&statusResult,0)==test && WIFEXITED(statusResult) && !WEXITSTATUS(statusResult));
    }
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
