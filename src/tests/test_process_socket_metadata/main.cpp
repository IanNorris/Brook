#include <cassert>
#include <cstdint>
#include "../../kernel/src/fd_table.h"
#include "../../kernel/src/socket_identity.h"
#include "../../kernel/src/user_namespace.h"

namespace brook {
struct Process {
    uint32_t tgid=1; bool isThread=false;
    uint64_t incarnation=1,leaderIncarnation=1;
    UserNamespace* userNamespace=nullptr;
    uint32_t uid=1000,euid=1000,suid=1000,gid=1000,egid=1000,sgid=1000;
    UserCapabilityState capabilities{};
    FdEntry* fds=nullptr;
    SpinLock lock{}, replacementLock{};
    SpinLock* selected=&lock;
};
static SpinLock g_credentialsLock;
static Process* current;
static Process* target;
static unsigned yields;
static bool changeOwnerOnYield,changeLockOnCopy;
static Process* ProcessCurrent() { return current; }
struct ProcessOperationGuard { explicit ProcessOperationGuard(Process*) {} };
static SpinLock* ProcessFdLock(Process* p) { return p->selected; }
static UserAuthority UserAuthorityLocked(Process* p) {
    return {p->userNamespace,p->euid,p->egid,p->capabilities.effective};
}
static bool SockGetIdentity(int slot,SocketIdentity* out) {
    assert(slot==2);
    assert(!SpinLockTryAcquire(&g_credentialsLock));
    assert(!SpinLockTryAcquire(&target->lock));
    *out={123,1000,1000};
    if(changeLockOnCopy)target->selected=&target->replacementLock;
    return true;
}
static void SchedulerYield() {
    // A contended FD owner must be able to acquire credentials to run. The
    // resumed task changes credentials before allowing our retried snapshot.
    assert(SpinLockTryAcquire(&g_credentialsLock));
    if(changeOwnerOnYield)target->euid=2000;
    SpinLockRelease(&g_credentialsLock);
    SpinLockRelease(&target->lock);++yields;
}
#include "../../kernel/src/process_socket_metadata.inc"
}

int main() {
    using namespace brook;
    FdEntry slots[MAX_FDS]{};Process actor,other;
    actor.capabilities={0,0,0,0};other.capabilities={0,0,0,0};
    other.tgid=2;other.incarnation=2;other.fds=slots;
    current=&actor;target=&other;SocketIdentity identity{};
    slots[7].type=FdType::Socket;slots[7].handle=reinterpret_cast<void*>(3);
    assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==1 && identity.inode==123);
    assert(ProcessReadSocketMetadata(&actor,&other,8,&identity)==-2);
    assert(ProcessReadSocketMetadata(&actor,&other,MAX_FDS,&identity)==-2);
    slots[7].closing=true;assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==-2);
    slots[7].closing=false;other.suid=2000;
    assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==-13);other.suid=1000;
    other.egid=2000;assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==-13);other.egid=1000;
    other.capabilities.permitted=1;
    assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==-13);other.capabilities.permitted=0;
    actor.euid=0;assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==1);actor.euid=1000;
    slots[7].type=FdType::Vnode;assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==0);
    slots[7].type=FdType::Socket;
    SpinLockAcquire(&other.lock);changeOwnerOnYield=true;
    assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==-13 && yields==1);
    other.euid=1000;changeOwnerOnYield=false;changeLockOnCopy=true;
    assert(ProcessReadSocketMetadata(&actor,&other,7,&identity)==1);
    assert(SpinLockTryAcquire(&other.lock));SpinLockRelease(&other.lock);
    assert(SpinLockTryAcquire(&other.replacementLock));SpinLockRelease(&other.replacementLock);
    assert(SpinLockTryAcquire(&g_credentialsLock));SpinLockRelease(&g_credentialsLock);
}
