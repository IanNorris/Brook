#include "fido_hidraw.h"

#include "devtmpfs.h"
#include "fd_table.h"
#include "module.h"
#include "scheduler.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "sync/kmutex.h"
#include "syscall_internal.h"
#include "tmpfs.h"
#include "user_copy.h"
#include "vfs.h"

namespace brook {

namespace {
constexpr uint32_t HidrawMajor = 240; // Brook-local dynamic character major.
constexpr unsigned MaxDevices = 8, MaxFiles = 32, QueueDepth = 16;

struct HidrawDevice {
    uint32_t identity;
    uint16_t vendor, product;
    unsigned descBytes;
    bool live, published, outgoing;
    HidrawQueueOutput output;
    void* cookie;
    uint8_t descriptor[HidrawDescriptorLimit];
};

// Registration, tmpfs publication and removal can sleep. The IRQ lock only
// protects the bounded registry and queues. The two locks are never acquired
// in reverse order, and no driver or VFS callback runs under the IRQ lock.
KMutex publishLock{};
IrqSpinLock stateLock{};
Tmpfs* deviceFs;
HidrawDevice devices[MaxDevices];
uint32_t nextIdentity = 1;

HidrawDevice* Find(uint32_t identity)
{
    if (!identity) return nullptr;
    for (auto& device : devices)
        if (device.identity == identity) return &device;
    return nullptr;
}

uint32_t Minor(uint64_t rdev)
{
    return (rdev & 0xff) | ((rdev >> 12) & 0xffffff00);
}

void NodeName(unsigned index, char (&name)[8])
{
    memcpy(name, "hidraw0", sizeof(name));
    name[6] += index;
}

void Publish(HidrawDevice& device)
{
    if (!deviceFs || device.published) return;
    const auto saved = IrqSpinLockAcquire(&stateLock);
    const bool live = device.live;
    IrqSpinLockRelease(&stateLock, saved);
    if (!live) return;
    char name[8];
    NodeName(&device - devices, name);
    int error = TmpfsSeedEntry(deviceFs, name, 0020600, DevEncode(HidrawMajor, device.identity),
                               nullptr, 1000, 1000);
    if (error) SerialPrintf("USB_FIDO_NODE name=%s error=%d\n", name, error);
    else SerialPrintf("USB_FIDO_NODE name=%s identity=%u\n", name, device.identity);
    device.published = !error;
}
} // namespace

struct HidrawFile {
    uint32_t refs;
    uint32_t identity;
    VnodeStat metadata;
    unsigned head, count;
    uint8_t reports[QueueDepth][HidrawReportBytes];
};

namespace {
HidrawFile files[MaxFiles];

int ReadReport(HidrawFile& file, uint8_t* out)
{
    const auto saved = IrqSpinLockAcquire(&stateLock);
    auto* device = Find(file.identity);
    int result = -EAGAIN;
    if (!device || !device->live) result = -ENODEV;
    else if (file.count) {
        memcpy(out, file.reports[file.head], HidrawReportBytes);
        file.head = (file.head + 1) % QueueDepth;
        --file.count;
        result = HidrawReportBytes;
    }
    IrqSpinLockRelease(&stateLock, saved);
    return result;
}
} // namespace

uint32_t HidrawAttach(uint16_t vendor, uint16_t product, const uint8_t* desc, unsigned size,
                      HidrawQueueOutput output, void* cookie)
{
    if (!desc || !size || size > HidrawDescriptorLimit || !output) return 0;
    const uintptr_t callback = reinterpret_cast<uintptr_t>(output);
    if (!ModuleRetainAddresses(&callback, 1)) return 0;
    KMutexLock(&publishLock);
    const auto saved = IrqSpinLockAcquire(&stateLock);
    HidrawDevice* found = nullptr;
    for (auto& device : devices) {
        if (!device.identity && nextIdentity) {
            device.identity = nextIdentity++;
            device.vendor = vendor;
            device.product = product;
            device.descBytes = size;
            device.output = output;
            device.cookie = cookie;
            memcpy(device.descriptor, desc, size);
            found = &device;
            break;
        }
    }
    IrqSpinLockRelease(&stateLock, saved);
    const uint32_t identity = found ? found->identity : 0;
    KMutexUnlock(&publishLock);
    return identity;
}

void HidrawActivate(uint32_t identity)
{
    KMutexLock(&publishLock);
    const auto saved = IrqSpinLockAcquire(&stateLock);
    auto* device = Find(identity);
    if (device) device->live = true;
    IrqSpinLockRelease(&stateLock, saved);
    if (device) Publish(*device);
    KMutexUnlock(&publishLock);
}

void HidrawDetach(uint32_t identity)
{
    KMutexLock(&publishLock);
    const auto saved = IrqSpinLockAcquire(&stateLock);
    auto* device = Find(identity);
    if (device) device->live = false;
    IrqSpinLockRelease(&stateLock, saved);
    if (device) {
        if (device->published) {
            char name[8];
            NodeName(device - devices, name);
            TmpfsRemoveCharDevice(deviceFs, name, DevEncode(HidrawMajor, identity));
        }
        const auto reset = IrqSpinLockAcquire(&stateLock);
        *device = {};
        IrqSpinLockRelease(&stateLock, reset);
    }
    KMutexUnlock(&publishLock);
}

void HidrawReceive(uint32_t identity, const uint8_t* report)
{
    const auto saved = IrqSpinLockAcquire(&stateLock);
    auto* device = Find(identity);
    if (device && device->live) {
        for (auto& file : files) {
            if (!file.refs || file.identity != identity || file.count == QueueDepth) continue;
            memcpy(file.reports[(file.head + file.count) % QueueDepth], report, HidrawReportBytes);
            ++file.count;
        }
    }
    IrqSpinLockRelease(&stateLock, saved);
}

void HidrawOutputComplete(uint32_t identity, bool ok)
{
    const auto saved = IrqSpinLockAcquire(&stateLock);
    if (auto* device = Find(identity)) {
        device->outgoing = false;
        if (!ok) device->live = false;
    }
    IrqSpinLockRelease(&stateLock, saved);
}

void HidrawTransportFailed(uint32_t identity)
{
    HidrawOutputComplete(identity, false);
}

void HidrawMount(Tmpfs* fs)
{
    KMutexLock(&publishLock);
    deviceFs = fs;
    for (auto& device : devices) Publish(device);
    KMutexUnlock(&publishLock);
}

bool HidrawDeviceNumber(uint64_t rdev)
{
    return ((rdev >> 8) & 0xfff) == HidrawMajor;
}

int HidrawOpen(Process* proc, const VnodeStat& stat, uint32_t flags)
{
    if ((flags & 3) == 3) return -EINVAL;
    const auto saved = IrqSpinLockAcquire(&stateLock);
    const auto identity = Minor(stat.rdev);
    const auto* device = Find(identity);
    HidrawFile* acquired = nullptr;
    int error = -ENODEV;
    if (device && device->live) {
        error = -EMFILE;
        for (auto& file : files) {
            if (file.refs) continue;
            file.refs = 1;
            file.identity = identity;
            file.metadata = stat;
            file.head = file.count = 0;
            acquired = &file;
            break;
        }
    }
    IrqSpinLockRelease(&stateLock, saved);
    if (!acquired) return error;
    const int fd = FdAlloc(proc, FdType::DevHidraw, acquired, flags, 0, (flags & 0x80000) != 0);
    if (fd < 0) HidrawUnref(acquired);
    return fd;
}

void HidrawRef(HidrawFile* file)
{
    const auto saved = IrqSpinLockAcquire(&stateLock);
    ++file->refs;
    IrqSpinLockRelease(&stateLock, saved);
}

void HidrawUnref(HidrawFile* file)
{
    const auto saved = IrqSpinLockAcquire(&stateLock);
    --file->refs;
    IrqSpinLockRelease(&stateLock, saved);
}

uint32_t HidrawReady(HidrawFile* file, uint32_t events)
{
    const auto saved = IrqSpinLockAcquire(&stateLock);
    const auto* device = Find(file->identity);
    constexpr uint32_t In = 1, Out = 4, Err = 8, Hup = 16;
    uint32_t ready = 0;
    if (!device || !device->live) ready = Err | Hup;
    else ready = events & ((file->count ? In : 0) | (!device->outgoing ? Out : 0));
    IrqSpinLockRelease(&stateLock, saved);
    return ready;
}

void HidrawStat(HidrawFile* file, VnodeStat& stat)
{
    stat = file->metadata;
}

int64_t HidrawRead(FdEntry* entry, uint64_t buffer, uint64_t bytes)
{
    if ((FdStatus(entry) & 3) == 1) return -EBADF;
    if (!bytes) return 0;
    if (bytes > HidrawReportBytes) bytes = HidrawReportBytes;
    if (!UserBufferWritable(buffer, bytes)) return -EFAULT;
    auto& file = *static_cast<HidrawFile*>(entry->handle);
    uint8_t report[HidrawReportBytes];
    while (true) {
        const int result = ReadReport(file, report);
        if (result > 0) {
            return CopyToUser(buffer, report, bytes) ? static_cast<int64_t>(bytes) : -EFAULT;
        }
        if (result != -EAGAIN || (FdStatus(entry) & 0x800)) return result;
        if (HasPendingSignals()) return -EINTR;
        SchedulerSleepMs(5);
    }
}

int64_t HidrawWrite(FdEntry* entry, uint64_t buffer, uint64_t bytes, bool user)
{
    if ((FdStatus(entry) & 3) == 0) return -EBADF;
    if (bytes != HidrawReportBytes + 1) return -EINVAL;
    uint8_t report[HidrawReportBytes + 1];
    if (user) {
        if (!CopyFromUser(report, buffer, bytes)) return -EFAULT;
    } else memcpy(report, reinterpret_cast<const void*>(buffer), bytes);
    if (report[0]) return -EINVAL; // Linux requires a zero report ID for unnumbered writes.
    auto& file = *static_cast<HidrawFile*>(entry->handle);
    while (true) {
        const auto saved = IrqSpinLockAcquire(&stateLock);
        auto* device = Find(file.identity);
        const bool live = device && device->live;
        HidrawQueueOutput queue = nullptr;
        void* cookie = nullptr;
        if (live && !device->outgoing) {
            device->outgoing = true;
            queue = device->output;
            cookie = device->cookie;
        }
        IrqSpinLockRelease(&stateLock, saved);
        if (!live) return -ENODEV;
        if (queue) {
            int error = queue(cookie, file.identity, report + 1);
            if (!error) return bytes;
            HidrawOutputComplete(file.identity, false);
            return error;
        }
        if (FdStatus(entry) & 0x800) return -EAGAIN;
        if (HasPendingSignals()) return -EINTR;
        SchedulerSleepMs(5);
    }
}

int64_t HidrawIoctl(HidrawFile* file, uint64_t cmd, uint64_t arg)
{
    constexpr uint64_t GetSize = 0x80044801, GetDesc = 0x90044802, GetInfo = 0x80084803;
    if (cmd != GetSize && cmd != GetDesc && cmd != GetInfo) return -25; // ENOTTY
    uint32_t limit = HidrawDescriptorLimit;
    if (cmd == GetDesc && !CopyFromUser(&limit, arg, sizeof(limit))) return -EFAULT;
    if (limit > 4096) return -EINVAL;

    struct RawInfo { uint32_t bus; uint16_t vendor, product; };
    RawInfo info{};
    uint8_t descriptor[HidrawDescriptorLimit];
    uint32_t length = 0;
    const auto saved = IrqSpinLockAcquire(&stateLock);
    const auto* device = Find(file->identity);
    if (device && device->live) {
        info = {3, device->vendor, device->product}; // BUS_USB
        length = device->descBytes;
        if (cmd == GetDesc) memcpy(descriptor, device->descriptor, length);
    }
    IrqSpinLockRelease(&stateLock, saved);
    if (!length) return -ENODEV;
    if (cmd == GetInfo) return CopyToUser(arg, &info, sizeof(info)) ? 0 : -EFAULT;
    if (cmd == GetSize) return CopyToUser(arg, &length, sizeof(length)) ? 0 : -EFAULT;
    if (limit > length) limit = length;
    return CopyToUser(arg + sizeof(limit), descriptor, limit) ? 0 : -EFAULT;
}

} // namespace brook
