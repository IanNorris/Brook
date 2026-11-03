#include "guard_repair_session.h"
#include "ahci_system_volume.h"
#include "device.h"
#include "memory/heap.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "vfs.h"

namespace brook {
namespace {
enum class Phase:uint8_t {Unused,Repair,RepairDone,Between,Verify,VerifyDone,Ended};
SpinLock lock{};Phase phase=Phase::Unused;Device* target=nullptr;
uint16_t group=0;uint64_t incarnation=0,bytes=0;uint32_t views=0,operations=0,writers=0;
uint16_t controllerGroup=0;uint64_t controllerIncarnation=0;bool tracked=false,polling=false;
bool Controller(const Process* p) {
    return p && p->isKernelThread && p->pid==p->tgid && p->tgid==controllerGroup &&
        p->incarnation==controllerIncarnation && p->incarnation;
}
struct View {bool writable;};
bool Match(const Process* p) {
    if(!p || p->tgid!=group)return false;
    return (p->pid==group?p->incarnation:p->leaderIncarnation)==incarnation;
}
bool Admit(bool write) {
    SpinLockAcquire(&lock);
    const bool ok=(phase==Phase::Repair || (!write && phase==Phase::Verify)) &&
        Match(ProcessCurrent()) && operations!=~uint32_t{0};
    if(ok)++operations;
    SpinLockRelease(&lock);return ok;
}
void Done() {SpinLockAcquire(&lock);--operations;SpinLockRelease(&lock);}
int Open(Vnode*,int) {return -13;} // no reopening through unrelated VFS paths
int Transfer(Vnode* node,void* data,uint64_t length,uint64_t* offset,bool write) {
    if(!node || !node->priv || !offset || (!data && length))return -22;
    auto* view=static_cast<View*>(node->priv);
    if(write && !view->writable)return -30;
    if(!Admit(write))return -13;
    int result=0;
    if(*offset>=bytes)result=write && length?-28:0;
    else if(length) {
        if(length>bytes-*offset)length=bytes-*offset;
        if(length>0x7ffff000ULL)length=0x7ffff000ULL;
        result=write?DeviceWrite(target,*offset,data,length):target->ops->read(target,*offset,data,length);
        if(result>=0 && (!result || uint64_t(result)>length))result=-5;
        if(result>0)*offset+=uint64_t(result);
    }
    Done();return result;
}
int Read(Vnode* n,void* p,uint64_t size,uint64_t* pos) {return Transfer(n,p,size,pos,false);}
int Write(Vnode* n,const void* p,uint64_t size,uint64_t* pos) {return Transfer(n,const_cast<void*>(p),size,pos,true);}
int Fsync(Vnode*) {if(!Admit(false))return -13;const int result=DeviceFlush(target);Done();return result;}
int Stat(Vnode* n,VnodeStat* out) {
    if(!n || !n->priv || !out)return -22;
    if(!Admit(false))return -13;
    *out={};out->size=bytes;out->mode=0100600;out->ino=1;Done();return 0;
}
int Access(Vnode*,uint8_t required,bool) {if(required&1)return -13;if(!Admit((required&2)!=0))return -13;Done();return 0;}
int Truncate(Vnode*,uint64_t) {return -30;}
int Times(Vnode*,const VnodeTime*) {return -30;}
int Chmod(Vnode*,uint16_t) {return -30;}
int Chown(Vnode*,uint32_t,uint32_t) {return -30;}
int Append(Vnode*,const void*,uint64_t,uint64_t*) {return -30;}
void Close(Vnode* node) {
    if(!node || !node->priv)return;
    auto* view=static_cast<View*>(node->priv);const bool writable=view->writable;kfree(view);
    SpinLockAcquire(&lock);--views;if(writable)--writers;SpinLockRelease(&lock);
}
const VnodeOps ops={.open=Open,.read=Read,.write=Write,.readdir=nullptr,.close=Close,
    .stat=Stat,.fsync=Fsync,.set_times=Times,.chmod=Chmod,.truncate=Truncate,
    .file_object=nullptr,.chown=Chown,.append=Append,.space=nullptr,.access=Access,
    .directory_path=nullptr,.deny_mapping=true};
}
bool GuardRepairBegin(Device* device,const Process* child,bool writable) {
    if(!child || !child->tgid || !device || !AhciSystemGuardIsTestData(device) ||
       !DeviceIsRegistered(device) || !device->ops || !device->ops->read || !device->ops->write ||
       ProcessReadIds(const_cast<Process*>(child),false).effective!=0)return false;
    const uint16_t id=child->tgid;const uint64_t birth=child->pid==id?child->incarnation:child->leaderIncarnation;
    const auto* block=reinterpret_cast<const BlockDeviceOps*>(device->ops);
    if(!birth || !block->block_count || !block->block_size || DeviceBlockSize(device)!=512)return false;
    const uint64_t blocks=DeviceBlockCount(device);if(!blocks || blocks>~uint64_t{0}/512)return false;
    SpinLockAcquire(&lock);
    const bool ok=!tracked && !views && !operations && ((writable && phase==Phase::Unused) || (!writable && phase==Phase::Between && target==device));
    if(ok) {target=device;group=id;incarnation=birth;bytes=blocks*512;phase=writable?Phase::Repair:Phase::Verify;}
    SpinLockRelease(&lock);return ok;
}
bool GuardRepairBeginTracked(Device* device,const Process* child,bool writable) {
    auto* current=ProcessCurrent();
    if(!current || !current->isKernelThread || current->pid!=current->tgid || !current->incarnation ||
       !child || child->pid!=child->tgid || !child->tgid || !child->incarnation || child->parentPid!=current->tgid ||
       ProcessReadIds(const_cast<Process*>(child),false).effective!=0 || !device || !AhciSystemGuardIsTestData(device) ||
       !DeviceIsRegistered(device) || !device->ops || !device->ops->read || !device->ops->write)return false;
    const auto* block=reinterpret_cast<const BlockDeviceOps*>(device->ops);
    if(!block->block_count || !block->block_size || DeviceBlockSize(device)!=512)return false;
    const uint64_t blocks=DeviceBlockCount(device);if(!blocks || blocks>~uint64_t{0}/512)return false;
    SpinLockAcquire(&lock);
    const bool fresh=writable && phase==Phase::Unused;
    const bool next=!writable && tracked && phase==Phase::Between && target==device && Controller(current);
    const bool ok=!polling && !views && !operations && (fresh || next);
    if(ok) {target=device;group=child->tgid;incarnation=child->incarnation;bytes=blocks*512;
        if(fresh){tracked=true;controllerGroup=current->tgid;controllerIncarnation=current->incarnation;}
        phase=writable?Phase::Repair:Phase::Verify;}
    SpinLockRelease(&lock);return ok;
}
int GuardRepairPollTracked(Device* device,uint16_t pid,uint64_t birth,ChildWaitSnapshot* result) {
    auto* current=ProcessCurrent();if(!result)return -13;
    SpinLockAcquire(&lock);
    const bool admitted=tracked && (phase==Phase::Repair || phase==Phase::Verify) && Controller(current) &&
        device==target && pid==group && birth==incarnation && !polling;
    if(admitted)polling=true;
    SpinLockRelease(&lock);if(!admitted)return -13;
    ChildWaitSnapshot observed{};const int waited=SchedulerWaitChild(current,1,pid,4,&observed);
    SpinLockAcquire(&lock);polling=false;
    if(phase!=Phase::Repair && phase!=Phase::Verify) {SpinLockRelease(&lock);return -125;}
    if(waited==1) {
        const bool preen=phase==Phase::Repair;
        const bool ok=observed.pid==pid && observed.incarnation==birth && observed.code==1 &&
            (observed.status==0 || (preen && observed.status==256)) && !views && !operations;
        phase=ok?(preen?Phase::RepairDone:Phase::VerifyDone):Phase::Ended;*result=observed;
    } else if(waited<0)phase=Phase::Ended;
    SpinLockRelease(&lock);return waited;
}
bool GuardRepairConsumeCompletion(Device* device,bool writable) {
    auto* current=ProcessCurrent();SpinLockAcquire(&lock);
    const bool ok=tracked && Controller(current) && device==target && !polling && !views && !operations &&
        phase==(writable?Phase::RepairDone:Phase::VerifyDone);
    if(ok)phase=writable?Phase::Between:Phase::Ended;
    SpinLockRelease(&lock);return ok;
}
Vnode* GuardRepairOpenLinux(uint64_t flags,int* error) {
    if(error)*error=-13;
    constexpr uint64_t allowed=3u|0x80u|0x800u|0x8000u|0x20000u|0x80000u;
    if((flags&~allowed) || ((flags&3)!=0 && (flags&3)!=2)) {if(error)*error=-30;return nullptr;}
    const bool writable=(flags&3)==2;
    SpinLockAcquire(&lock);
    const bool admitted=(phase==Phase::Repair || (!writable && phase==Phase::Verify)) && Match(ProcessCurrent()) &&
        views!=~uint32_t{0} && (!writable || !writers);
    if(admitted) {++views;if(writable)++writers;}
    SpinLockRelease(&lock);
    if(!admitted)return nullptr;
    auto* view=static_cast<View*>(kmalloc(sizeof(View)));
    auto* node=view?static_cast<Vnode*>(kmalloc(sizeof(Vnode))):nullptr;
    SpinLockAcquire(&lock);
    const bool commit=node && (phase==Phase::Repair || (!writable && phase==Phase::Verify)) && Match(ProcessCurrent());
    SpinLockRelease(&lock);
    if(!commit) {
        if(node)kfree(node);
        if(view)kfree(view);
        SpinLockAcquire(&lock);--views;if(writable)--writers;SpinLockRelease(&lock);
        if(error)*error=node?-13:-12;
        return nullptr;
    }
    *view={writable};*node={};node->ops=&ops;node->type=VnodeType::File;node->priv=view;node->refCount=1;
    if(error)*error=0;
    return node;
}
bool GuardRepairEnd() {
    SpinLockAcquire(&lock);
    const bool clean=!tracked && (phase==Phase::Repair || phase==Phase::Verify) && !views && !operations;
    phase=clean && phase==Phase::Repair?Phase::Between:Phase::Ended;
    SpinLockRelease(&lock);return clean;
}
void GuardRepairAbort() {SpinLockAcquire(&lock);phase=Phase::Ended;SpinLockRelease(&lock);}
bool GuardRepairQuiescent() {
    SpinLockAcquire(&lock);const bool quiet=phase==Phase::Ended && !polling && !views && !operations;
    SpinLockRelease(&lock);return quiet;
}
}
