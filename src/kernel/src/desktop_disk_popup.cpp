#include "desktop_disk_popup.h"
#include "apic.h"
#include "memory/heap.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "sync/ksemaphore.h"
#include "vfs.h"
#include "vfs_path.h"
#include "compositor.h"
#include "string.h"

namespace brook {
namespace {

struct Volume {
    DesktopDiskVolume display;
    DesktopDiskIdentity storage;
};

static bool g_started;
static bool g_visible;
static bool g_wakeQueued;
static uint32_t g_request;
static KSemaphore g_open;
static SpinLock g_snapshotLock;
static Volume* g_volumes;
static uint32_t g_volumeCount;
static bool g_measured;

bool Visible() { return __atomic_load_n(&g_visible, __ATOMIC_ACQUIRE); }

void Publish(Volume* volumes, uint32_t count)
{
    SpinLockAcquire(&g_snapshotLock);
    Volume* old = g_volumes;
    g_volumes = volumes;
    __atomic_store_n(&g_volumeCount, count, __ATOMIC_RELAXED);
    g_measured = true;
    SpinLockRelease(&g_snapshotLock);
    if (old) kfree(old);
    CompositorWake();
}

void CopyMountName(DesktopDiskVolume& entry, const char* path)
{
    const uint32_t capacity = sizeof(entry.path);
    uint32_t i = 0;
    for (; i + 1 < capacity && path[i]; ++i) entry.path[i] = path[i];
    if (path[i]) {
        // The kernel only sees safe text in the model; preserve complete
        // code points at the boundary rather than splitting a UTF-8 label.
        for (i -= 3; i && (uint8_t(path[i]) & 0xc0) == 0x80; --i) {}
        for (unsigned dots = 0; dots < 3; ++dots) entry.path[i++] = '.';
    }
    entry.path[i] = 0;
}

void Measure(uint32_t request)
{
    VfsPath root{};
    (void)VfsPathRoot(&root);
    const uint32_t cap = VfsMountMaxSlots();
    auto* volumes = cap ? static_cast<Volume*>(kmalloc(uint64_t(cap) * sizeof(Volume))) : nullptr;
    if (cap && !volumes) { VfsPathRelease(root); return; }
    uint32_t count = 0;
    for (uint32_t i = 0; i < cap && Visible(); ++i) {
        const auto meta = VfsMountSnapshotAt(i);
        if (!meta.used || !DesktopDiskVolumeVisible(meta.mountPoint, meta.fsType, meta.systemInternal)) continue;

        VfsSpace space{};
        bool available = false;
        if (auto* node = root.object ? VfsOpenAt(root, root, meta.mountPoint, VFS_O_METADATA, 0, 0, nullptr) : nullptr) {
            available = VfsVnodeIsPublishedUserVolume(node, meta.publication, meta.drive) &&
                        VfsSpaceInfo(node, &space) == 0;
            VfsClose(node);
        }
        // This also checks mounts whose drivers cannot open a retained root
        // or answer statfs. They stay listed without a fabricated capacity.
        if (!VfsUserVolumeStillPublished(meta)) continue;

        Volume candidate{};
        CopyMountName(candidate.display, meta.mountPoint);
        memcpy(candidate.storage.fsType, meta.fsType, sizeof(candidate.storage.fsType));
        candidate.storage.drive = meta.drive;
        if (available) {
            candidate.storage.identity = space.identity;
            candidate.storage.type = space.type;
            candidate.display.available = DesktopDiskCapacity(space.blockSize, space.blocks, space.availableBlocks,
                                                               candidate.display.totalBytes, candidate.display.freeBytes);
        }
        uint32_t slot = 0;
        while (slot < count && !DesktopDiskSameVolume(volumes[slot].storage, candidate.storage)) ++slot;
        if (slot == count) volumes[count++] = candidate;
        else if ((candidate.display.available && !volumes[slot].display.available) ||
                 (candidate.display.available == volumes[slot].display.available &&
                  (strlen(candidate.display.path) < strlen(volumes[slot].display.path) ||
                   (strlen(candidate.display.path) == strlen(volumes[slot].display.path) &&
                    strcmp(candidate.display.path, volumes[slot].display.path) < 0))))
            volumes[slot] = candidate;
    }
    VfsPathRelease(root);
    if (!Visible() || request != __atomic_load_n(&g_request, __ATOMIC_ACQUIRE)) {
        if (volumes) kfree(volumes);
        return;
    }
    for (uint32_t i = 1; i < count; ++i) {
        const Volume row = volumes[i];
        uint32_t j = i;
        while (j && strcmp(row.display.path, volumes[j - 1].display.path) < 0) {
            volumes[j] = volumes[j - 1];
            --j;
        }
        volumes[j] = row;
    }
    Publish(volumes, count);
}

void Worker(void*)
{
    uint32_t served = 0;
    uint64_t last = 0;
    for (;;) {
        while (!Visible()) {
            KSemaphoreWait(&g_open);
            __atomic_store_n(&g_wakeQueued, false, __ATOMIC_RELEASE);
        }
        const auto request = __atomic_load_n(&g_request, __ATOMIC_ACQUIRE);
        const auto now = ApicTickCount();
        if (request != served || now - last >= 4000) {
            Measure(request);
            last = ApicTickCount();
            served = request;
        }
        SchedulerSleepMs(250);
    }
}
} // namespace

void DesktopDiskWorkerStart()
{
    if (__atomic_exchange_n(&g_started, true, __ATOMIC_ACQ_REL)) return;
    Process* worker = KernelThreadCreate("disk-volumes", Worker, nullptr, 3);
    if (worker) SchedulerAddProcess(worker);
    else __atomic_store_n(&g_started, false, __ATOMIC_RELEASE);
}

void DesktopDiskSetVisible(bool visible)
{
    if (__atomic_exchange_n(&g_visible, visible, __ATOMIC_ACQ_REL) == visible || !visible) return;
    __atomic_add_fetch(&g_request, uint32_t{1}, __ATOMIC_RELEASE);
    if (!__atomic_exchange_n(&g_wakeQueued, true, __ATOMIC_ACQ_REL)) KSemaphoreSignal(&g_open);
}

uint32_t DesktopDiskVolumeCount()
{
    return __atomic_load_n(&g_volumeCount, __ATOMIC_RELAXED);
}

uint32_t DesktopDiskReadPage(uint32_t first, uint32_t capacity, DesktopDiskPage& out)
{
    if (capacity > DiskPopupPageRows) capacity = DiskPopupPageRows;
    SpinLockAcquire(&g_snapshotLock);
    out.total = __atomic_load_n(&g_volumeCount, __ATOMIC_RELAXED);
    out.measured = g_measured;
    out.first = DesktopDiskPageFirst(first, out.total, capacity);
    const uint32_t remaining = out.total - out.first;
    out.count = remaining < capacity ? remaining : capacity;
    for (uint32_t i = 0; i < out.count; ++i) out.rows[i] = g_volumes[out.first + i].display;
    SpinLockRelease(&g_snapshotLock);
    return out.first;
}
} // namespace brook
