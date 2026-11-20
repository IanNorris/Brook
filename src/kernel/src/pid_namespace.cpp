#include "pid_namespace.h"
#include "memory/heap.h"
#include "process.h"
#include "process_user_namespace.h"
#include "spinlock.h"

namespace brook {
struct PidNamespace {
    const PidNamespace* parent;
    UserNamespace* owner;
    uint64_t tag;
    mutable uint32_t references;
    unsigned depth;
    mutable PidIdAllocator ids;
    mutable uint32_t tasks;
    bool initPublished;
    bool dying;
    bool draining;
};

namespace {
IrqSpinLock pidViewLock;
uint64_t nextViewTag = 1;

PidNamespace* Create(const PidNamespace* parent, Process* ownerTask)
{
    auto* ns = static_cast<PidNamespace*>(kmalloc(sizeof(PidNamespace)));
    if (!ns) return nullptr;
    PidNamespaceRetain(parent);
    const uint64_t tag = __atomic_fetch_add(&nextViewTag, 1, __ATOMIC_RELAXED);
    if (!tag) __builtin_trap();
    *ns = {parent, ProcessUserNamespaceAcquire(ownerTask), tag, 1,
           parent ? parent->depth + 1 : 1, {}, 0, false, false, false};
    return ns;
}

bool MayReserve(const PidNamespace* active)
{
    for (const auto* ns = active; ns; ns = ns->parent)
        if (ns->dying || (ns != active && !ns->initPublished)) return false;
    return true;
}

void ReleaseTaskIds(PidNamespaceIds& identity)
{
    while (identity.count) {
        auto& entry = identity.entries[--identity.count];
        PidIdAllocator::Release(entry.groupLease);
        PidIdAllocator::Release(entry.sessionLease);
        PidIdAllocator::Release(entry.tgidLease);
        PidIdAllocator::Release(entry.tidLease);
        entry = {};
    }
    identity.published = false;
}
}

const PidNamespace* ProcessPidView(const Process* process)
{
    return process ? process->pidNamespaceIds.Active() : nullptr;
}

uint32_t ProcessVisibleTid(const Process* process, const PidNamespace* view)
{
    return process ? process->pidNamespaceIds.VisibleTid(view, process->pid) : 0;
}

uint32_t ProcessVisibleTgid(const Process* process, const PidNamespace* view)
{
    return process ? process->pidNamespaceIds.VisibleTgid(
        view, process->tgid ? process->tgid : process->pid) : 0;
}

uint32_t ProcessVisibleGroup(const Process* process, const PidNamespace* view, bool session)
{
    return process->pidNamespaceIds.VisibleRelation(view, session ? process->sid : process->pgid, session);
}

void ProcessSetGroupView(Process* process, const Process* source, bool session, bool newGroup)
{
    const uint16_t id = newGroup ? (source->tgid ? source->tgid : source->pid)
        : (session ? source->sid : source->pgid);
    (session ? process->sid : process->pgid) = id;
    auto& identity = process->pidNamespaceIds;
    if (!identity.count) return;
    const auto flags = IrqSpinLockAcquire(&pidViewLock);
    for (unsigned i = 0; i < identity.count; ++i) {
        auto& entry = identity.entries[i];
        auto& current = session ? entry.session : entry.group;
        auto& lease = session ? entry.sessionLease : entry.groupLease;
        const uint32_t next = newGroup ? ProcessVisibleTgid(source, entry.ns) :
            ProcessVisibleGroup(source, entry.ns, session);
        if (current == next) continue;
        PidIdAllocator::Release(lease);
        if (next) entry.ns->ids.Retain(lease, next);
        current = next;
    }
    IrqSpinLockRelease(&pidViewLock, flags);
}

UserNamespace* PidNamespaceOwner(const PidNamespace* view)
{
    return view ? view->owner : nullptr;
}

bool PidNamespaceContains(const PidNamespace* ancestor, const PidNamespace* view)
{
    if (!ancestor) return true;
    for (; view; view = view->parent)
        if (view == ancestor) return true;
    return false;
}

uint64_t PidNamespaceTag(const PidNamespace* view)
{
    return view ? view->tag : 0;
}

PidGroupIdentity ProcessPidGroupIdentity(const Process* process)
{
    PidGroupIdentity identity{};
    identity.global = ProcessVisibleTgid(process, nullptr);
    if (!process) return identity;
    identity.count = process->pidNamespaceIds.count;
    for (unsigned i = 0; i < identity.count; ++i) {
        const auto& entry = process->pidNamespaceIds.entries[i];
        identity.entries[i] = {PidNamespaceTag(entry.ns), entry.tgid};
    }
    return identity;
}

void PidNamespaceRetain(const PidNamespace* view)
{
    if (view) __atomic_add_fetch(&view->references, 1, __ATOMIC_RELAXED);
}

void PidNamespaceRelease(const PidNamespace* view)
{
    while (view && __atomic_sub_fetch(&view->references, 1, __ATOMIC_ACQ_REL) == 0) {
        const auto* parent = view->parent;
        UserNamespaceRelease(view->owner);
        kfree(const_cast<PidNamespace*>(view));
        view = parent;
    }
}

namespace {
enum class BirthKind { Child, NamespaceInit, InjectedThread };

int PrepareChild(Process* child, const Process* parent, BirthKind kind)
{
    const bool newNamespace = kind == BirthKind::NamespaceInit;
    const bool injected = kind == BirthKind::InjectedThread;
    if (injected && !child->isThread) return -22;
    const auto* current = ProcessPidView(parent);
    if (newNamespace && current && current->depth == PidNamespaceIds::MaxDepth) return -28;
    const auto* fresh = newNamespace ? Create(current, child) : nullptr;
    if (newNamespace && !fresh) return -12;

    const auto flags = IrqSpinLockAcquire(&pidViewLock);
    const auto* selected = parent->pidNamespaceForChildren;
    const auto* active = fresh ? fresh : injected ? current : selected;
    int error = 0;
    if (!injected && (child->isThread || newNamespace) && selected != current) error = -22;
    else if (!MayReserve(active)) error = -12;
    if (error || !active) {
        IrqSpinLockRelease(&pidViewLock, flags);
        PidNamespaceRelease(fresh);
        return error;
    }
    const PidNamespace* ancestors[PidNamespaceIds::MaxDepth];
    unsigned count = 0;
    for (auto* ns = active; ns; ns = ns->parent) ancestors[count++] = ns;
    while (count) {
        const auto* view = ancestors[--count];
        auto& ids = child->pidNamespaceIds;
        auto& entry = ids.entries[ids.count];
        // Only PID 1 can be reserved before the first task publishes. An
        // aborted birth returns it without touching the regular cursor.
        const uint32_t tid = view->ids.Reserve(entry.tidLease, !view->initPublished);
        if (!tid) {
            ReleaseTaskIds(ids);
            IrqSpinLockRelease(&pidViewLock, flags);
            PidNamespaceRelease(fresh);
            return -12;
        }
        const uint32_t tgid = child->isThread ? ProcessVisibleTgid(parent, view) : tid;
        if (!ids.Add(view, tid, tgid)) __builtin_trap();
        if (child->isThread) view->ids.Retain(entry.tgidLease, tgid);
    }
    if (!fresh) PidNamespaceRetain(active);
    PidNamespaceRetain(active);
    child->pidNamespaceForChildren = active;
    IrqSpinLockRelease(&pidViewLock, flags);
    return 0;
}

}

int ProcessPidPrepareChild(Process* child, const Process* parent, bool newNamespace)
{
    return PrepareChild(child, parent, newNamespace ? BirthKind::NamespaceInit : BirthKind::Child);
}

int ProcessPidPrepareInjectedThread(Process* child, const Process* target)
{
    return PrepareChild(child, target, BirthKind::InjectedThread);
}

bool ProcessPidPublish(Process* process, bool runnable)
{
    const auto* active = ProcessPidView(process);
    if (!active) return true;
    const auto flags = IrqSpinLockAcquire(&pidViewLock);
    bool live = true;
    for (const auto* view = active; view; view = view->parent)
        if (view->dying) live = false;
    if (live) {
        if (process->pidNamespaceIds.published) __builtin_trap();
        process->pidNamespaceIds.published = true;
        for (const auto* view = active; view; view = view->parent)
            __atomic_add_fetch(&view->tasks, 1, __ATOMIC_RELAXED);
        if (runnable && ProcessVisibleTid(process, active) == 1)
            const_cast<PidNamespace*>(active)->initPublished = true;
    }
    IrqSpinLockRelease(&pidViewLock, flags);
    return live;
}

void ProcessPidRelease(Process* process)
{
    const auto* active = ProcessPidView(process);
    if (active) {
        const auto flags = IrqSpinLockAcquire(&pidViewLock);
        if (process->pidNamespaceIds.published) {
            for (const auto* view = active; view; view = view->parent)
                __atomic_sub_fetch(&view->tasks, 1, __ATOMIC_RELEASE);
        }
        ReleaseTaskIds(process->pidNamespaceIds);
        IrqSpinLockRelease(&pidViewLock, flags);
    }
    const auto* selected = process->pidNamespaceForChildren;
    process->pidNamespaceForChildren = nullptr;
    PidNamespaceRelease(selected);
    PidNamespaceRelease(active);
}

int ProcessPidUnshare(Process* process)
{
    const auto* active = ProcessPidView(process);
    if (active && active->depth == PidNamespaceIds::MaxDepth) return -28;
    auto* fresh = Create(active, process);
    if (!fresh) return -12;
    const auto flags = IrqSpinLockAcquire(&pidViewLock);
    if (process->pidNamespaceForChildren != active) {
        IrqSpinLockRelease(&pidViewLock, flags);
        PidNamespaceRelease(fresh);
        return -22;
    }
    process->pidNamespaceForChildren = fresh;
    IrqSpinLockRelease(&pidViewLock, flags);
    PidNamespaceRelease(active);
    return 0;
}

bool ProcessPidIsNamespaceInit(const Process* process)
{
    const auto* active = ProcessPidView(process);
    return active && ProcessVisibleTgid(process, active) == 1;
}

bool PidNamespaceStopBirths(const PidNamespace* view)
{
    if (!view) return false;
    const auto flags = IrqSpinLockAcquire(&pidViewLock);
    auto* state = const_cast<PidNamespace*>(view);
    const bool started = !state->dying && state->initPublished;
    if (started) {
        state->dying = true;
        __atomic_store_n(&state->draining, true, __ATOMIC_RELEASE);
    }
    IrqSpinLockRelease(&pidViewLock, flags);
    return started;
}

bool PidNamespaceDraining(const PidNamespace* view)
{
    return view && __atomic_load_n(&view->draining, __ATOMIC_ACQUIRE);
}

uint32_t PidNamespaceTaskCount(const PidNamespace* view)
{
    return __atomic_load_n(&view->tasks, __ATOMIC_ACQUIRE);
}

void PidNamespaceFinishDrain(const PidNamespace* view)
{
    __atomic_store_n(&const_cast<PidNamespace*>(view)->draining, false, __ATOMIC_RELEASE);
}
}
