#include "usb_system_fsck.h"
#include "usb_system_fsck_policy.h"
#include "device.h"
#include "memory/heap.h"
#include "process.h"
#include "spinlock.h"
#include "vfs.h"

namespace brook {
namespace {
enum class Phase : uint8_t { Unused, Active, Ended };
SpinLock lock{};
Phase phase = Phase::Unused;
Device* target = nullptr;
uint16_t group = 0;
uint64_t incarnation = 0, bytes = 0;
uint32_t views = 0, operations = 0, writers = 0;
UsbSystemFsckTranscript<8192> transcript;
SpinLock transcriptLock{};

struct View { bool writable; };

bool Match(const Process* p) {
    if (!p || p->tgid != group) return false;
    return (p->pid == group ? p->incarnation : p->leaderIncarnation) == incarnation;
}
bool Admit() {
    SpinLockAcquire(&lock);
    const bool ok = phase == Phase::Active && Match(ProcessCurrent()) && operations != ~uint32_t{0};
    if (ok) ++operations;
    SpinLockRelease(&lock);
    return ok;
}
void Done() { SpinLockAcquire(&lock); --operations; SpinLockRelease(&lock); }

int Open(Vnode*, int) { return -13; }  // never reachable through another path
int Transfer(Vnode* node, void* data, uint64_t length, uint64_t* offset, bool write) {
    if (!node || !node->priv || !offset || (!data && length)) return -22;
    if (write && !static_cast<View*>(node->priv)->writable) return -9;
    if (!Admit()) return -13;
    int result = 0;
    if (*offset >= bytes) result = write && length ? -28 : 0;
    else if (length) {
        if (length > bytes - *offset) length = bytes - *offset;
        if (length > 0x7ffff000ULL) length = 0x7ffff000ULL;
        // The adapter bounds every transfer to the validated extent and
        // routes writes through the BROOK_SYSTEM lease and write window.
        result = write ? DeviceWrite(target, *offset, data, length)
                       : target->ops->read(target, *offset, data, length);
        if (result >= 0 && (!result || uint64_t(result) > length)) result = -5;
        if (result > 0) *offset += uint64_t(result);
    }
    Done();
    return result;
}
int Read(Vnode* n, void* p, uint64_t size, uint64_t* pos) { return Transfer(n, p, size, pos, false); }
int Write(Vnode* n, const void* p, uint64_t size, uint64_t* pos) {
    return Transfer(n, const_cast<void*>(p), size, pos, true);
}
int Fsync(Vnode*) {
    if (!Admit()) return -13;
    const int result = DeviceFlush(target);
    Done();
    return result;
}
int Stat(Vnode* n, VnodeStat* out) {
    if (!n || !n->priv || !out) return -22;
    if (!Admit()) return -13;
    *out = {};
    out->size = bytes;
    out->mode = 0100600;
    out->ino = 1;
    Done();
    return 0;
}
int Access(Vnode*, uint8_t required, bool) {
    if (required & 1) return -13;
    if (!Admit()) return -13;
    Done();
    return 0;
}
int Truncate(Vnode*, uint64_t) { return -30; }
int Times(Vnode*, const VnodeTime*) { return -30; }
int Chmod(Vnode*, uint16_t) { return -30; }
int Chown(Vnode*, uint32_t, uint32_t) { return -30; }
int Append(Vnode*, const void*, uint64_t, uint64_t*) { return -30; }
void Close(Vnode* node) {
    if (!node || !node->priv) return;
    auto* view = static_cast<View*>(node->priv);
    const bool writable = view->writable;
    kfree(view);
    SpinLockAcquire(&lock);
    --views;
    if (writable) --writers;
    SpinLockRelease(&lock);
}
const VnodeOps ops = {.open = Open, .read = Read, .write = Write, .readdir = nullptr, .close = Close,
    .stat = Stat, .fsync = Fsync, .set_times = Times, .chmod = Chmod, .truncate = Truncate,
    .file_object = nullptr, .chown = Chown, .append = Append, .space = nullptr, .access = Access,
    .directory_path = nullptr, .deny_mapping = true};
}

bool UsbSystemFsckSessionBegin(Device* adapter, const Process* child) {
    if (!adapter || !child || !child->tgid || child->pid != child->tgid || !child->incarnation ||
        !DeviceIsRegistered(adapter) || !adapter->ops || !adapter->ops->read || !adapter->ops->write ||
        DeviceBlockSize(adapter) != 512 ||
        ProcessReadIds(const_cast<Process*>(child), false).effective != 0) return false;
    const uint64_t blocks = DeviceBlockCount(adapter);
    if (!blocks || blocks > ~uint64_t{0} / 512) return false;
    SpinLockAcquire(&lock);
    const bool ok = phase == Phase::Unused && !views && !operations;
    if (ok) {
        target = adapter;
        group = child->tgid;
        incarnation = child->incarnation;
        bytes = blocks * 512;
        phase = Phase::Active;
    }
    SpinLockRelease(&lock);
    return ok;
}

Vnode* UsbSystemFsckOpenLinux(uint64_t flags, int* error) {
    if (error) *error = -13;
    // O_ACCMODE, O_EXCL, O_NONBLOCK, O_LARGEFILE, O_NOFOLLOW, O_CLOEXEC.
    constexpr uint64_t allowed = 3u | 0x80u | 0x800u | 0x8000u | 0x20000u | 0x80000u;
    if ((flags & ~allowed) || (flags & 3) == 3) { if (error) *error = -22; return nullptr; }
    const bool writable = (flags & 3) != 0;
    SpinLockAcquire(&lock);
    const bool admitted = phase == Phase::Active && Match(ProcessCurrent()) && views != ~uint32_t{0} &&
        (!writable || !writers);
    if (admitted) { ++views; if (writable) ++writers; }
    SpinLockRelease(&lock);
    if (!admitted) return nullptr;
    auto* view = static_cast<View*>(kmalloc(sizeof(View)));
    auto* node = view ? static_cast<Vnode*>(kmalloc(sizeof(Vnode))) : nullptr;
    SpinLockAcquire(&lock);
    const bool commit = node && phase == Phase::Active && Match(ProcessCurrent());
    SpinLockRelease(&lock);
    if (!commit) {
        if (node) kfree(node);
        if (view) kfree(view);
        SpinLockAcquire(&lock);
        --views;
        if (writable) --writers;
        SpinLockRelease(&lock);
        if (error) *error = node ? -13 : -12;
        return nullptr;
    }
    *view = {writable};
    *node = {};
    node->ops = &ops;
    node->type = VnodeType::File;
    node->priv = view;
    node->refCount = 1;
    if (error) *error = 0;
    return node;
}

bool UsbSystemFsckSessionEnd() {
    SpinLockAcquire(&lock);
    phase = Phase::Ended;
    const bool quiet = !views && !operations;
    SpinLockRelease(&lock);
    return quiet;
}

void UsbSystemFsckCapture(const Process* writer, const char* text, uint32_t length) {
    if (!text || !length) return;
    SpinLockAcquire(&lock);
    const bool ours = phase == Phase::Active && Match(writer);
    SpinLockRelease(&lock);
    if (!ours) return;
    SpinLockAcquire(&transcriptLock);
    transcript.Append(text, length);
    SpinLockRelease(&transcriptLock);
}

const char* UsbSystemFsckTranscriptText(uint32_t* length, bool* truncated) {
    SpinLockAcquire(&transcriptLock);
    if (length) *length = transcript.length;
    if (truncated) *truncated = transcript.truncated;
    SpinLockRelease(&transcriptLock);
    return transcript.text;
}
}
