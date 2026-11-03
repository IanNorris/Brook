#include "guard_checker_session.h"
#include "guard_readonly_view.h"
#include "ahci_system_volume.h"
#include "device.h"
#include "memory/heap.h"
#include "process.h"
#include "spinlock.h"
#include "vfs.h"

namespace brook {
namespace {
enum class Phase : uint8_t {Unused,Active,Aborted};
struct Key {uint16_t group;uint64_t incarnation;};
SpinLock lock{};Phase phase=Phase::Unused;Device* target=nullptr;Key owner{};
uint32_t views=0,reads=0;
bool ProcessKey(const Process* p,Key& key) {
    if(!p || !p->tgid)return false;
    key={p->tgid,p->pid==p->tgid?p->incarnation:p->leaderIncarnation};
    return key.incarnation!=0;
}
bool Matches(const Process* p) {
    Key current{};return ProcessKey(p,current) && current.group==owner.group && current.incarnation==owner.incarnation;
}
struct View {Vnode* inner;};
bool Allowed() {
    SpinLockAcquire(&lock);const bool allowed=phase==Phase::Active && Matches(ProcessCurrent());
    SpinLockRelease(&lock);return allowed;
}
int Open(Vnode*,int flags) {return flags!=VFS_O_READ?-30:Allowed()?0:-13;}
int Read(Vnode* node,void* out,uint64_t length,uint64_t* offset) {
    if(!node || !node->priv)return -22;
    SpinLockAcquire(&lock);
    const bool admitted=phase==Phase::Active && Matches(ProcessCurrent()) && reads!=~uint32_t{0};
    if(admitted)++reads;
    SpinLockRelease(&lock);
    if(!admitted)return -13;
    auto* view=static_cast<View*>(node->priv);
    const int result=view->inner->ops->read(view->inner,out,length,offset);
    SpinLockAcquire(&lock);--reads;SpinLockRelease(&lock);return result;
}
int Write(Vnode*,const void*,uint64_t,uint64_t*) {return -30;}
int Truncate(Vnode*,uint64_t) {return -30;}
int Times(Vnode*,const VnodeTime*) {return -30;}
int Chmod(Vnode*,uint16_t) {return -30;}
int Chown(Vnode*,uint32_t,uint32_t) {return -30;}
int Append(Vnode*,const void*,uint64_t,uint64_t*) {return -30;}
int Access(Vnode*,uint8_t required,bool) {return required&2?-30:required&1?-13:Allowed()?0:-13;}
int Stat(Vnode* node,VnodeStat* out) {
    if(!node || !node->priv)return -22;
    if(!Allowed())return -13;
    auto* view=static_cast<View*>(node->priv);return view->inner->ops->stat(view->inner,out);
}
void Close(Vnode* node) {
    if(!node || !node->priv)return;
    auto* view=static_cast<View*>(node->priv);
    VfsRelease(view->inner);kfree(view);
    SpinLockAcquire(&lock);--views;SpinLockRelease(&lock);
}
const VnodeOps ops={.open=Open,.read=Read,.write=Write,.readdir=nullptr,.close=Close,
    .stat=Stat,.fsync=nullptr,.set_times=Times,.chmod=Chmod,.truncate=Truncate,
    .file_object=nullptr,.chown=Chown,.append=Append,.space=nullptr,.access=Access,
    .directory_path=nullptr,.deny_mapping=true};
}
bool GuardCheckerBegin(Device* device,const Process* checker) {
    Key key{};
    if(!ProcessKey(checker,key) || ProcessReadIds(const_cast<Process*>(checker),false).effective!=0 ||
       !device || !AhciSystemGuardIsReservedDevice(device) ||
       !DeviceIsRegistered(device) || !device->ops || !device->ops->ioctl ||
       device->ops->ioctl(device,DEVICE_IOCTL_READ_ONLY,nullptr)!=1)return false;
    SpinLockAcquire(&lock);const bool ok=phase==Phase::Unused;
    if(ok) {target=device;owner=key;phase=Phase::Active;}
    SpinLockRelease(&lock);return ok;
}
Vnode* GuardCheckerOpen(int flags,int* error) {
    if(error)*error=-13;
    if(flags!=VFS_O_READ) {if(error)*error=-30;return nullptr;}
    SpinLockAcquire(&lock);
    const bool admitted=phase==Phase::Active && Matches(ProcessCurrent()) && views!=~uint32_t{0};
    Device* device=target;if(admitted)++views;
    SpinLockRelease(&lock);
    if(!admitted)return nullptr;
    int innerError=0;Vnode* inner=GuardCreateReadOnlyView(device,VFS_O_READ,&innerError);
    auto* view=inner?static_cast<View*>(kmalloc(sizeof(View))):nullptr;
    auto* node=view?static_cast<Vnode*>(kmalloc(sizeof(Vnode))):nullptr;
    SpinLockAcquire(&lock);const bool commit=node && phase==Phase::Active && Matches(ProcessCurrent());
    SpinLockRelease(&lock);
    if(!commit) {
        const int failure=node?-13:inner?-12:innerError;
        if(node)kfree(node);
        if(view)kfree(view);
        if(inner)VfsRelease(inner);
        SpinLockAcquire(&lock);--views;SpinLockRelease(&lock);
        if(error)*error=failure;
        return nullptr;
    }
    *view={inner};*node={};node->ops=&ops;node->type=VnodeType::File;
    node->priv=view;node->refCount=1;node->cacheId=0;
    if(error)*error=0;
    return node;
}
Vnode* GuardCheckerOpenLinux(uint64_t flags,int* error) {
    // O_LARGEFILE, O_NONBLOCK, O_NOFOLLOW and O_CLOEXEC do not grant write
    // access. Reject every other bit, including O_PATH and unknown flags.
    constexpr uint64_t allowed=0x8000u|0x800u|0x20000u|0x80000u;
    if(flags&~allowed) {if(error)*error=-30;return nullptr;}
    return GuardCheckerOpen(VFS_O_READ,error);
}
void GuardCheckerAbort() {SpinLockAcquire(&lock);phase=Phase::Aborted;SpinLockRelease(&lock);}
GuardCheckerStatus GuardCheckerGetStatus() {
    SpinLockAcquire(&lock);const GuardCheckerStatus status{phase==Phase::Active,views,reads};
    SpinLockRelease(&lock);return status;
}
}
