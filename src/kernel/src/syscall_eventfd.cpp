// syscall_eventfd.cpp -- eventfd objects and eventfd2.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "syscall_internal.h"
#include "syscall_eventfd.h"
#include "syscall_handlers.h"

namespace brook {

uint64_t g_eventFdIdentity = 0;
static void EventFdInit(EventFdData* efd, uint64_t counter, uint32_t flags) {
    __builtin_memset(efd, 0, sizeof(*efd));
    efd->counter = counter;
    efd->flags = flags;
    efd->refCount = 1;
    efd->identity = __atomic_add_fetch(&g_eventFdIdentity, 1, __ATOMIC_RELAXED);
}
void EventFdWakeLocked(EventFdData* efd) {
    for (auto* n = efd->waiters; n; n = n->next) WakeProcess(n->process);
}

bool EventFdWaitGroup::add(EventFdData* efd, Process* process) {
    for (auto* n = nodes; n; n = n->nextGroup) if (n->efd == efd) return true;
    auto* n = static_cast<EventFdWaitNode*>(kmalloc(sizeof(EventFdWaitNode)));
    if (!n) return false;
    EventFdRef(efd);
    ProcessRef(process);
    {
        EventFdGuard guard(efd);
        *n = {efd, process, efd->waiters, nodes};
        efd->waiters = n;
    }
    nodes = n;
    return true;
}

EventFdWaitGroup::~EventFdWaitGroup() {
    while (nodes) {
        auto* n = nodes;
        nodes = n->nextGroup;
        {
            EventFdGuard guard(n->efd);
            auto** link = &n->efd->waiters;
            while (*link && *link != n) link = &(*link)->next;
            if (*link) *link = n->next;
        }
        ProcessUnref(n->process);
        EventFdUnref(n->efd);
        kfree(n);
    }
}

EventFdSnapshot EventFdReady(EventFdData* efd, uint32_t events) {
    EventFdGuard guard(efd);
    uint32_t ready = 0;
    if (efd->counter && (events & 1)) ready |= 1;
    if (efd->counter < ~0ULL - 1 && (events & 4)) ready |= 4;
    return {efd->identity, efd->inGeneration, efd->outGeneration, ready};
}
static constexpr uint32_t EFD_CLOEXEC = 0x80000;

void EventFdHandleRef(void* handle) { EventFdRef(static_cast<EventFdData*>(handle)); }

// ---------------------------------------------------------------------------
// sys_eventfd2 (290) — event notification file descriptor
// ---------------------------------------------------------------------------

int64_t sys_eventfd2(uint64_t initval, uint64_t flagsVal, uint64_t,
                       uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    const uint32_t flags=static_cast<uint32_t>(flagsVal);
    if(flags & ~(EFD_SEMAPHORE|EFD_CLOEXEC|EFD_NONBLOCK))return -EINVAL;
    KernelOperationGuard operation(proc);

    auto* efd = static_cast<EventFdData*>(kmalloc(sizeof(EventFdData)));
    if (!efd) return -ENOMEM;

    EventFdInit(efd, static_cast<uint32_t>(initval), flags);

    FdEntry entry{};entry.type=FdType::EventFd;entry.handle=efd;
    entry.fdFlags=(flags&EFD_CLOEXEC)?1:0;
    int fd=FdPublishNew(proc,entry,2u|((flags&EFD_NONBLOCK)?0x800u:0u));
    if (fd < 0) { kfree(efd); return -EMFILE; }

    DbgPrintf("sys_eventfd2: fd=%d initval=%lu flags=0x%lx\n", fd, initval, flagsVal);
    return fd;
}

} // namespace brook
