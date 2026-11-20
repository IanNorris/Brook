#include "posix_mqueue.h"
#include "posix_mqueue_order.h"

#include "apic.h"
#include "mount_namespace.h"
#include "vfs_object.h"
#include "posix_timer.h"
#include "process_fs.h"
#include "process_user_namespace.h"
#include "string.h"
#include "syscall_handlers.h"
#include "syscall_internal.h"
#include "syscall_time.h"
#include "timeout_value.h"
#include "user_copy.h"

namespace brook {
namespace {
struct MessageQueue;

constexpr unsigned maxQueues = 64;
constexpr int EACCES = 13, ENOSPC = 28, EMSGSIZE = 90;
constexpr unsigned maxMessages = MqueueOrder::MaxMessages;
constexpr unsigned maxMessageSize = 8192;
constexpr unsigned priorityCount = 32768;
constexpr unsigned nameBytes = 256;
constexpr uint32_t nonblock = 0x800;
constexpr uint32_t createFlag = 0x40;
constexpr uint32_t exclusiveFlag = 0x80;
constexpr uint32_t closeOnExec = 0x80000;
constexpr auto noDeadline = ~uint64_t{0};

struct MqAttr {
    int64_t flags, maxMessages, messageSize, messages;
    int64_t reserved[4];
};
static_assert(sizeof(MqAttr) == 64);
}

struct MqueueDomain {
    UserNamespace* owner;
    MessageQueue* first;
    uint32_t references;
    uint64_t identity;
};

namespace {
struct MessageQueue {
    MessageQueue* next;
    MqueueDomain* domain;
    uint64_t identity;
    unsigned references;
    uint32_t uid, gid;
    uint16_t mode;
    unsigned capacity, messageSize;
    MqueueOrder order;
    char name[nameBytes];

    char* Data(unsigned slot) { return reinterpret_cast<char*>(this + 1) + slot * messageSize; }
};

struct Waiter {
    Waiter* next;
    Process* task;
    MessageQueue* queue;
    uint64_t serial;
    bool sending, notified;
};

IrqSpinLock queueLock{};
MqueueDomain initial{nullptr, nullptr, 0, 1};
Waiter* waiters = nullptr;
uint64_t nextIdentity = 2;
uint64_t waiterSerial = 0;
unsigned allocated = 0;

struct Actor {
    uint32_t uid, gid;
    bool ownerOverride;
    bool accessOverride;
    SupplementaryGroups* groups;

    Actor(Process* task, MqueueDomain* domain)
        : uid(ProcessReadIds(task, false).effective), gid(ProcessReadIds(task, true).effective),
          ownerOverride(ProcessCapable(task, domain->owner, CapFowner)),
          accessOverride(ProcessCapable(task, domain->owner, CapDacOverride)),
          groups(ProcessGroupsAcquire(task)) {}
    ~Actor() { ProcessGroupsRelease(groups); }

    bool InGroup(uint32_t group) const {
        if (gid == group) return true;
        if (groups) for (unsigned i = 0; i < groups->count; ++i)
            if (groups->ids[i] == group) return true;
        return false;
    }

    bool Allows(const MessageQueue* queue, unsigned required) const {
        const unsigned shift = uid == queue->uid ? 6 : InGroup(queue->gid) ? 3 : 0;
        return accessOverride || ((queue->mode >> shift) & required) == required;
    }
};

MessageQueue** Find(MqueueDomain* domain, const char* name) {
    auto** entry = &domain->first;
    while (*entry && !StrEq((*entry)->name, name)) entry = &(*entry)->next;
    return entry;
}

void RetainDomain(MqueueDomain* domain) {
    if (domain != &initial) __atomic_add_fetch(&domain->references, 1, __ATOMIC_RELAXED);
}

void ReleaseDomain(MqueueDomain* domain) {
    if (domain != &initial && __atomic_sub_fetch(&domain->references, 1, __ATOMIC_ACQ_REL) == 0) {
        UserNamespaceRelease(domain->owner);
        kfree(domain);
    }
}

void Release(MessageQueue* queue) {
    auto irq = IrqSpinLockAcquire(&queueLock);
    const bool final = --queue->references == 0;
    if (final) --allocated;
    IrqSpinLockRelease(&queueLock, irq);
    if (final) { ReleaseDomain(queue->domain); kfree(queue); }
}

int ReadQueueName(uint64_t address, char (&name)[nameBytes]) {
    if (!CopyUserCString(address, name, sizeof(name))) return -EFAULT;
    if (!name[0]) return -ENOENT;
    if (StrEq(name, ".") || StrEq(name, "..")) return -EINVAL;
    for (const char* c = name; *c; ++c)
        if (*c == '/') return -EACCES;
    return 0;
}

void QueueStat(MessageQueue* queue, VnodeStat* stat) {
    *stat = {};
    stat->mode = 0100000 | queue->mode;
    stat->uid = queue->uid;
    stat->gid = queue->gid;
    stat->ino = queue->identity;
    stat->dev = queue->domain->identity;
}

int StatVnode(Vnode* vnode, VnodeStat* stat) {
    QueueStat(static_cast<MessageQueue*>(vnode->priv), stat);
    return 0;
}

void CloseVnode(Vnode* vnode) {
    Release(static_cast<MessageQueue*>(vnode->priv));
}

int ReadVnode(Vnode* vnode, void* buffer, uint64_t length, uint64_t* offset) {
    auto* queue = static_cast<MessageQueue*>(vnode->priv);
    auto irq = IrqSpinLockAcquire(&queueLock);
    unsigned bytes = queue->order.Bytes();
    IrqSpinLockRelease(&queueLock, irq);
    char text[80] = "QSIZE:";
    char digits[10];
    unsigned count = 0;
    do { digits[count++] = char('0' + bytes % 10); bytes /= 10; } while (bytes);
    unsigned end = 6;
    while (count) text[end++] = digits[--count];
    constexpr char tail[] = " NOTIFY:0 SIGNO:0 NOTIFY_PID:0\n";
    memcpy(text + end, tail, sizeof(tail) - 1);
    end += sizeof(tail) - 1;
    if (*offset >= end) return 0;
    if (length > end - *offset) length = end - *offset;
    memcpy(buffer, text + *offset, length);
    *offset += length;
    return static_cast<int>(length);
}

const VnodeOps queueOps = {
    .open = nullptr, .read = ReadVnode, .write = nullptr, .readdir = nullptr,
    .close = CloseVnode, .stat = StatVnode, .fsync = nullptr, .deny_mapping = true,
};

int RootStat(MqueueDomain* domain, VnodeStat* stat) {
    *stat = {};
    stat->isDir = true;
    stat->mode = 0041777;
    stat->ino = 1;
    stat->dev = domain->identity;
    stat->nlink = 2;
    return 0;
}

int RootVnodeStat(Vnode* vnode, VnodeStat* stat) {
    return RootStat(static_cast<MqueueDomain*>(vnode->priv), stat);
}

int RootReaddir(Vnode* vnode, DirEntry* result, uint32_t* cookie) {
    auto* domain = static_cast<MqueueDomain*>(vnode->priv);
    auto irq = IrqSpinLockAcquire(&queueLock);
    MessageQueue* next = nullptr;
    for (auto* queue = domain->first; queue; queue = queue->next) {
        if (queue->identity > *cookie && (!next || queue->identity < next->identity))
            next = queue;
    }
    if (next) {
        *result = {};
        strcpy(result->name, next->name);
        *cookie = static_cast<uint32_t>(next->identity);
    }
    IrqSpinLockRelease(&queueLock, irq);
    return next ? 1 : 0;
}

int QueueSpace(Vnode* vnode, VfsSpace* out) {
    *out = {};
    auto irq = IrqSpinLockAcquire(&queueLock);
    out->files = maxQueues;
    out->freeFiles = maxQueues - allocated;
    IrqSpinLockRelease(&queueLock, irq);
    out->type = 0x19800202; // MQUEUE_MAGIC
    out->blockSize = out->fragmentSize = 4096;
    out->maxName = nameBytes - 1;
    out->identity = static_cast<MqueueDomain*>(vnode->priv)->identity;
    return 0;
}

const VnodeOps rootVnodeOps = {
    .open = nullptr, .read = nullptr, .write = nullptr, .readdir = RootReaddir,
    .close = [](Vnode* node) { ReleaseDomain(static_cast<MqueueDomain*>(node->priv)); },
    .stat = RootVnodeStat, .fsync = nullptr, .space = QueueSpace,
};

int MakeRoot(void* context, uint8_t, VfsObject** out);
int LookupQueue(VfsObject* root, const char* name, VfsObject** out, bool);

const VfsObjectOps rootObjectOps = {
    .destroy = [](VfsObject* object) { ReleaseDomain(static_cast<MqueueDomain*>(object->priv)); kfree(object); },
    .stat = [](VfsObject* object,VnodeStat* stat) { return RootStat(static_cast<MqueueDomain*>(object->priv),stat); },
    .access = [](VfsObject*,uint8_t,bool) { return 0; },
    .lookup = LookupQueue,
    .parent = [](VfsObject* object,VfsObject** out,bool) { return MakeRoot(object->priv,0,out); },
    .readlink = nullptr,
    .open = [](VfsObject* object,int flags,int* error) -> Vnode* {
        if (flags & VFS_O_WRITE) { *error = -21; return nullptr; }
        auto* node = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!node) { *error = -ENOMEM; return nullptr; }
        auto* domain = static_cast<MqueueDomain*>(object->priv);
        RetainDomain(domain);
        *node = {&rootVnodeOps,VnodeType::Dir,domain,1,0};
        return node;
    },
    .remove = [](VfsObject* root,const char* name,bool directory) {
        if (directory) return -20;
        auto* domain = static_cast<MqueueDomain*>(root->priv);
        Actor actor(ProcessCurrent(),domain);
        auto irq = IrqSpinLockAcquire(&queueLock);
        auto** slot = Find(domain,name);
        auto* queue = *slot;
        const int error = !queue ? -ENOENT : actor.uid != queue->uid && !actor.ownerOverride ? -EACCES : 0;
        if (!error) *slot = queue->next;
        IrqSpinLockRelease(&queueLock,irq);
        if (!error) Release(queue);
        return error;
    },
    .directory_path = [](VfsObject*,char* out,uint32_t capacity) -> int {
        if (capacity < 2) return -ERANGE;
        strcpy(out,"/");
        return 0;
    },
};

int QueueAccess(VfsObject* object, uint8_t required, bool) {
    auto* queue = static_cast<MessageQueue*>(object->priv);
    Actor actor(ProcessCurrent(), queue->domain);
    return (required & 1) || !actor.Allows(queue, required) ? -EACCES : 0;
}

const VfsObjectOps queueObjectOps = {
    .destroy = [](VfsObject* object) { Release(static_cast<MessageQueue*>(object->priv)); kfree(object); },
    .stat = [](VfsObject* object,VnodeStat* stat) {
        QueueStat(static_cast<MessageQueue*>(object->priv),stat);
        return 0;
    },
    .access = QueueAccess,
    .lookup = nullptr,
    .parent = nullptr,
    .readlink = nullptr,
    .open = [](VfsObject* object,int flags,int* error) -> Vnode* {
        const unsigned required = flags & VFS_O_METADATA ? 0 :
            flags & VFS_O_READWRITE ? 6 : flags & VFS_O_WRITE ? 2 : 4;
        if ((*error = QueueAccess(object,required,false))) return nullptr;
        auto* node = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!node) { *error = -ENOMEM; return nullptr; }
        auto* queue = static_cast<MessageQueue*>(object->priv);
        auto irq = IrqSpinLockAcquire(&queueLock);
        ++queue->references;
        IrqSpinLockRelease(&queueLock,irq);
        *node = {&queueOps,VnodeType::File,queue,1,0};
        return node;
    },
};

int MakeRoot(void* context, uint8_t, VfsObject** out) {
    auto* object = static_cast<VfsObject*>(kmalloc(sizeof(VfsObject)));
    if (!object) return -ENOMEM;
    auto* domain = static_cast<MqueueDomain*>(context);
    RetainDomain(domain);
    *object = {&rootObjectOps,domain,domain->identity,1};
    *out = object;
    return 0;
}

int LookupQueue(VfsObject* root,const char* name,VfsObject** out,bool) {
    auto* object = static_cast<VfsObject*>(kmalloc(sizeof(VfsObject)));
    if (!object) return -ENOMEM;
    auto irq = IrqSpinLockAcquire(&queueLock);
    auto* queue = *Find(static_cast<MqueueDomain*>(root->priv),name);
    if (queue) ++queue->references;
    IrqSpinLockRelease(&queueLock,irq);
    if (!queue) { kfree(object); return -ENOENT; }
    *object = {&queueObjectOps,queue,queue->identity,1};
    *out = object;
    return 0;
}

int VnodeObject(Vnode* node,VfsObject** out) {
    if (node->ops == &rootVnodeOps) return MakeRoot(node->priv,0,out);
    if (node->ops != &queueOps) return -EINVAL;
    auto* object = static_cast<VfsObject*>(kmalloc(sizeof(VfsObject)));
    if (!object) return -ENOMEM;
    auto* queue = static_cast<MessageQueue*>(node->priv);
    auto irq = IrqSpinLockAcquire(&queueLock);
    ++queue->references;
    IrqSpinLockRelease(&queueLock,irq);
    *object = {&queueObjectOps,queue,queue->identity,1};
    *out = object;
    return 0;
}

const VfsFsOps fsOps = {
    .mount = [](uint8_t,void**) { return false; },
    .unmount = [](void* context) { ReleaseDomain(static_cast<MqueueDomain*>(context)); return 0; },
    .open = nullptr, .stat_path = nullptr, .lstat_path = nullptr, .unlink = nullptr,
    .mkdir = nullptr, .rename = nullptr, .symlink = nullptr, .readlink = nullptr, .sync = nullptr,
    .root_object = MakeRoot, .vnode_object = VnodeObject,
};

MessageQueue* GetQueue(FdEntry* entry) {
    if (!entry || entry->type != FdType::Vnode) return nullptr;
    auto* vnode = static_cast<Vnode*>(entry->handle);
    return vnode && vnode->ops == &queueOps ? static_cast<MessageQueue*>(vnode->priv) : nullptr;
}

void WakeWaiters(MessageQueue* queue, bool sending) {
    auto irq = IrqSpinLockAcquire(&queueLock);
    const uint64_t cutoff = waiterSerial;
    for (;;) {
        Process* task = nullptr;
        for (auto* w = waiters; w; w = w->next) {
            if (w->queue != queue || w->sending != sending || w->notified || w->serial > cutoff)
                continue;
            w->notified = true;
            task = w->task;
            ProcessRef(task);
            break;
        }
        // Keep the short-lived target pin protected until after waking it.
        IrqSpinLockRelease(&queueLock, irq & ~0x200ULL);
        if (task) {
            WakeProcess(task);
            ProcessUnref(task);
        }
        if (irq & 0x200) __asm__ volatile("sti" ::: "memory");
        if (!task) return;
        irq = IrqSpinLockAcquire(&queueLock);
    }
}

void Enqueue(MessageQueue* queue, const char* data, unsigned size, unsigned priority) {
    const unsigned slot = queue->order.Push(size, priority);
    memcpy(queue->Data(slot), data, size);
}

unsigned Dequeue(MessageQueue* queue, char* data, unsigned* priority) {
    const auto message = queue->order.Pop();
    memcpy(data, queue->Data(message.slot), message.length);
    *priority = message.priority;
    return message.length;
}

int ReadDeadline(uint64_t address, uint64_t* deadline) {
    *deadline = noDeadline;
    if (!address) return 0;
    timespec time{};
    if (!CopyFromUser(&time, address, sizeof(time))) return -EFAULT;
    return TimeoutNanoseconds(time.tv_sec, time.tv_nsec, deadline) ? 0 : -EINVAL;
}

int Transfer(Process* task, OpenFile* file, MessageQueue* queue, bool send,
             char* contents, unsigned length, unsigned* priority, uint64_t deadline) {
    for (;;) {
        auto irq = IrqSpinLockAcquire(&queueLock);
        if (send ? queue->order.count < queue->capacity : queue->order.count != 0) {
            const int size = send ? 0 : Dequeue(queue, contents, priority);
            if (send) Enqueue(queue, contents, length, *priority);
            IrqSpinLockRelease(&queueLock, irq);
            WakeWaiters(queue, !send);
            return size;
        }
        if (OpenFileStatus(file) & nonblock) {
            IrqSpinLockRelease(&queueLock, irq);
            return -EAGAIN;
        }
        if (HasPendingSignals() || __atomic_load_n(&task->terminalPending, __ATOMIC_ACQUIRE)) {
            IrqSpinLockRelease(&queueLock, irq);
            return -EINTR;
        }
        const uint64_t now = deadline == noDeadline ? 0 : ClockNowNs(0);
        if (now >= deadline) {
            IrqSpinLockRelease(&queueLock, irq);
            return -ETIMEDOUT;
        }
        Waiter waiter{waiters, task, queue, ++waiterSerial, send, false};
        waiters = &waiter;
        __atomic_store_n(&task->pendingWakeup, 0, __ATOMIC_RELEASE);
        task->wakeupTick = deadline == noDeadline ? 0 :
            TimeoutAddSafe(ApicTickCount(), (deadline - now) / 1000000 + 1);
        IrqSpinLockRelease(&queueLock, irq);
        SchedulerBlockInterruptible(task);
        irq = IrqSpinLockAcquire(&queueLock);
        auto** link = &waiters;
        while (*link != &waiter) link = &(*link)->next;
        *link = waiter.next;
        IrqSpinLockRelease(&queueLock, irq);
    }
}

int64_t UserTransfer(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t priorityValue,
                     uint64_t deadlineAddress, bool sending) {
    auto* task = ProcessCurrent();
    if (!task) return -EBADF;
    KernelOperationGuard operation(task);
    FdRef reference(task, static_cast<int>(fd));
    auto* queue = GetQueue(reference.fde);
    if (!queue) return -EBADF;
    const unsigned access = FdStatus(reference.fde) & 3;
    if (sending ? !access : access == 1) return -EBADF;
    if (sending ? length > queue->messageSize : length < queue->messageSize) return -EMSGSIZE;
    if (sending && priorityValue >= priorityCount) return -EINVAL;
    uint64_t deadline;
    if (int error = ReadDeadline(deadlineAddress, &deadline)) return error;
    const unsigned size = sending ? static_cast<unsigned>(length) : queue->messageSize;
    auto* staged = static_cast<char*>(kmalloc(size ? size : 1));
    if (!staged) return -ENOMEM;
    unsigned priority = static_cast<unsigned>(priorityValue);
    const bool accessible = sending ?
        (!size || CopyFromUser(staged, buffer, size)) :
        (UserBufferWritable(buffer, size) &&
            (!priorityValue || UserBufferWritable(priorityValue, sizeof(priority))));
    int result = accessible ? Transfer(task, reference.fde->file, queue, sending,
                                       staged, size, &priority, deadline) : -EFAULT;
    if (!sending && result >= 0 &&
        ((result && !CopyToUser(buffer, staged, result)) ||
         (priorityValue && !CopyToUser(priorityValue, &priority, sizeof(priority)))))
        result = -EFAULT;
    kfree(staged);
    return result;
}
} // namespace

MqueueDomain* MqueueInitialDomain() { return &initial; }

MqueueDomain* MqueueDomainCreate(UserNamespace* owner) {
    auto* domain = static_cast<MqueueDomain*>(kmalloc(sizeof(MqueueDomain)));
    if (!domain) return nullptr;
    UserNamespaceRetain(owner);
    auto irq = IrqSpinLockAcquire(&queueLock);
    *domain = {owner, nullptr, 1, nextIdentity++};
    IrqSpinLockRelease(&queueLock, irq);
    return domain;
}

void MqueueDomainRetire(MqueueDomain* domain) {
    if (!domain || domain == &initial) return;
    auto irq = IrqSpinLockAcquire(&queueLock);
    auto* queue = domain->first;
    domain->first = nullptr;
    IrqSpinLockRelease(&queueLock, irq);
    while (queue) {
        auto* next = queue->next;
        Release(queue);
        queue = next;
    }
    ReleaseDomain(domain);
}

bool MqueueRegisterFilesystem() { return VfsRegisterFs("mqueue",&fsOps); }

int MqueueMount(Process* process,MountNamespace* ns,const VfsPath& target,uint64_t flags) {
    auto* domain = ProcessMqueueDomain(process);
    if (!ProcessCapable(process,domain->owner,CapSysAdmin)) return -EPERM;
    RetainDomain(domain);
    const auto restrictions = flags | MountNoDev | MountNoExec | MountNoSuid;
    int error = VfsMountNamespaceMountFresh(ns,target,"mqueue",&fsOps,domain,0,restrictions);
    if (error) ReleaseDomain(domain);
    return error;
}

int64_t sys_mq_open(uint64_t address, uint64_t flags, uint64_t mode,
                    uint64_t attrAddress, uint64_t, uint64_t) {
    if ((flags & 3) == 3 || flags & ~(uint64_t{3} | nonblock | createFlag | exclusiveFlag | closeOnExec))
        return -EINVAL;
    auto* task = ProcessCurrent();
    if (!task) return -ESRCH;
    KernelOperationGuard operation(task);
    char name[nameBytes];
    if (int error = ReadQueueName(address, name)) return error;

    auto* domain = ProcessMqueueDomain(task);
    Actor actor(task, domain);
    const auto fs = ProcessFsAcquire(task);
    const uint16_t mask = FsContextReadMask(fs);
    FsContextRelease(fs);
    auto* vnode = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
    if (!vnode) return -ENOMEM;
    auto* file = OpenFileCreate(static_cast<uint32_t>(flags));
    if (!file) { kfree(vnode); return -ENOMEM; }

    MqAttr attr{0, maxMessages, maxMessageSize, 0, {}};
    MessageQueue* prepared = nullptr;
    int result;
    for (;;) {
        auto irq = IrqSpinLockAcquire(&queueLock);
        auto** slot = Find(domain, name);
        auto* queue = *slot;
        const unsigned access = flags & 3;
        if (queue && (flags & (createFlag | exclusiveFlag)) == (createFlag | exclusiveFlag))
            result = -EEXIST;
        else if (queue && !actor.Allows(queue, access == 0 ? 4 : access == 1 ? 2 : 6))
            result = -EACCES;
        else if (!queue && !(flags & createFlag))
            result = -ENOENT;
        else if (!queue && allocated >= maxQueues)
            result = -ENOSPC;
        else if (!queue && !prepared) {
            IrqSpinLockRelease(&queueLock, irq);
            if (attrAddress && !CopyFromUser(&attr, attrAddress, sizeof(attr))) { result = -EFAULT; break; }
            if (attr.maxMessages <= 0 || attr.maxMessages > maxMessages ||
                attr.messageSize <= 0 || attr.messageSize > maxMessageSize) { result = -EINVAL; break; }
            prepared = static_cast<MessageQueue*>(kmalloc(
                sizeof(MessageQueue) + attr.maxMessages * attr.messageSize));
            if (!prepared) { result = -ENOMEM; break; }
            *prepared = {};
            prepared->uid = actor.uid;
            prepared->gid = actor.gid;
            prepared->mode = static_cast<uint16_t>(mode & ~mask & 0777);
            prepared->capacity = static_cast<unsigned>(attr.maxMessages);
            prepared->messageSize = static_cast<unsigned>(attr.messageSize);
            strcpy(prepared->name, name);
            continue;
        } else {
            auto* opened = queue ? queue : prepared;
            if (!queue) {
                opened->identity = nextIdentity++;
                opened->domain = domain;
            }
            *vnode = {&queueOps, VnodeType::File, opened, 1, 0};
            FdEntry fd{};
            fd.type = FdType::Vnode;
            fd.handle = vnode;
            fd.fdFlags = 1;
            result = FdPublishPrepared(task, fd, file);
            if (result >= 0) {
                ++opened->references;
                if (!queue) {
                    prepared = nullptr;
                    RetainDomain(domain);
                    ++opened->references;
                    ++allocated;
                    *slot = opened;
                }
            }
        }
        IrqSpinLockRelease(&queueLock, irq);
        break;
    }
    kfree(prepared);
    if (result < 0) { OpenFileRelease(file); kfree(vnode); }
    return result;
}

int64_t sys_mq_unlink(uint64_t address, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = ProcessCurrent();
    if (!task) return -ESRCH;
    KernelOperationGuard operation(task);
    char name[nameBytes];
    if (int error = ReadQueueName(address, name)) return error;
    auto* domain = ProcessMqueueDomain(task);
    Actor actor(task, domain);
    auto irq = IrqSpinLockAcquire(&queueLock);
    auto** slot = Find(domain, name);
    auto* queue = *slot;
    const int error = !queue ? -ENOENT : actor.uid != queue->uid && !actor.ownerOverride ? -EACCES : 0;
    if (!error) *slot = queue->next;
    IrqSpinLockRelease(&queueLock, irq);
    if (!error) Release(queue);
    return error;
}

int64_t sys_mq_timedsend(uint64_t fd, uint64_t payload, uint64_t length,
                         uint64_t priority, uint64_t deadline, uint64_t) {
    return UserTransfer(fd, payload, length, priority, deadline, true);
}

int64_t sys_mq_timedreceive(uint64_t fd, uint64_t payload, uint64_t length,
                            uint64_t priority, uint64_t deadline, uint64_t) {
    return UserTransfer(fd, payload, length, priority, deadline, false);
}

int64_t sys_mq_getsetattr(uint64_t fd, uint64_t requested, uint64_t previous,
                          uint64_t, uint64_t, uint64_t) {
    auto* task = ProcessCurrent();
    if (!task) return -EBADF;
    KernelOperationGuard operation(task);
    FdRef reference(task, static_cast<int>(fd));
    auto* queue = GetQueue(reference.fde);
    if (!queue) return -EBADF;
    MqAttr incoming{};
    if (requested && !CopyFromUser(&incoming, requested, sizeof(incoming))) return -EFAULT;
    if (incoming.flags & ~int64_t(nonblock)) return -EINVAL;
    auto irq = IrqSpinLockAcquire(&queueLock);
    MqAttr old{FdStatus(reference.fde) & nonblock, queue->capacity, queue->messageSize,
               queue->order.count, {}};
    if (requested) OpenFileUpdateStatus(reference.fde->file, nonblock, incoming.flags);
    IrqSpinLockRelease(&queueLock, irq);
    return previous && !CopyToUser(previous, &old, sizeof(old)) ? -EFAULT : 0;
}
} // namespace brook
