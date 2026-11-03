#include <cassert>
#include <cstdint>
#include "../../kernel/src/fd_table.h"

namespace brook {
struct VnodeStat {
    uint64_t size=0, ino=0, blocks=0, rdev=0, dev=1;
    uint32_t nlink=1;
    uint16_t mode=0;
    bool exactDescriptorMetadata=false;
};
struct Process {
    uint32_t tgid=1; bool isThread=false;
    uint64_t incarnation=1, leaderIncarnation=1;
    FdEntry* fds=nullptr; SpinLock lock{}, replacementLock{};
    SpinLock* selected=&lock;
};
static Process* current;
static Process* target;
static unsigned refs, guards;
static bool reuseOnStat;
static Process* ProcessCurrent() { return current; }
static SpinLock* ProcessFdLock(Process* p) { return p->selected; }
struct ProcessOperationGuard {
    explicit ProcessOperationGuard(Process*) { ++guards; }
    ~ProcessOperationGuard() { --guards; }
};
static void MemFdHandleRef(void* handle) {
    assert(handle == reinterpret_cast<void*>(1));
    assert(!SpinLockTryAcquire(&target->lock)); ++refs;
}
static void MemFdHandleStat(void* handle, VnodeStat* out) {
    assert(handle == reinterpret_cast<void*>(1) && refs == 1 && guards == 1);
    assert(SpinLockTryAcquire(&target->lock));
    if (reuseOnStat) {
        target->fds[7].type = FdType::Pipe;
        target->fds[7].handle = nullptr;
        target->selected = &target->replacementLock;
    }
    SpinLockRelease(&target->lock);
    *out = {}; out->size = 8193; out->ino = 17; out->exactDescriptorMetadata = true;
}
static void MemFdHandleUnref(void* handle) {
    assert(handle == reinterpret_cast<void*>(1) && refs == 1 && guards == 1);
    assert(SpinLockTryAcquire(&target->lock)); SpinLockRelease(&target->lock); --refs;
}
#include "../../kernel/src/process_anonymous_metadata.inc"
}

int main() {
    using namespace brook;
    FdEntry fds[MAX_FDS]{}; Process actor, other;
    current=&actor; target=&other; other.fds=fds;
    VnodeStat st{};
    assert(ProcessReadAnonymousFdMetadata(&actor,&other,7,&st)==-2);
    fds[7].type=FdType::MemFd; fds[7].handle=reinterpret_cast<void*>(1);
    fds[7].closing=1; assert(ProcessReadAnonymousFdMetadata(&actor,&other,7,&st)==-2);
    fds[7].closing=0; other.incarnation=2;
    assert(ProcessReadAnonymousFdMetadata(&actor,&other,7,&st)==-13 && refs==0);
    other.incarnation=1; reuseOnStat=true;
    assert(ProcessReadAnonymousFdMetadata(&actor,&other,7,&st)==1);
    assert(st.ino==17 && st.size==8193 && refs==0 && guards==0);
    assert(SpinLockTryAcquire(&other.lock)); SpinLockRelease(&other.lock);
    assert(SpinLockTryAcquire(&other.replacementLock)); SpinLockRelease(&other.replacementLock);
    fds[7].flags=0; assert(ProcessReadAnonymousFdMetadata(&actor,&other,7,&st)==1 && st.mode==0010666);
    fds[7].flags=4; assert(ProcessReadAnonymousFdMetadata(&actor,&other,7,&st)==1 && st.mode==0020666 && st.rdev==0x8800);
    fds[7].type=FdType::SyntheticMem; assert(ProcessReadAnonymousFdMetadata(&actor,&other,7,&st)==0);
    return 0;
}
