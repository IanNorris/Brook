// Boot-stick update gate: see src/shared/brook_boot_stick.h.
//
// Inert unless the initrd carries /etc/boot-stick-update and the loader
// handed over a BootStick volume identity. The only write authority it ever
// grants is a USB write window over the one partition the firmware loaded
// this boot from, and only between ARM(RW) and DISARM.
#include "boot_stick.h"
#include "boot_timing_runtime.h"
#include "boot_protocol/boot_protocol.h"
#include "device.h"
#include "fatfs_glue.h"
#include "gpt.h"
#include "mount_namespace.h"
#include "serial.h"
#include "sync/kmutex.h"
#include "kprintf.h"
#include "fatfs_glue.h"
#include "vfs.h"
#include "../../shared/brook_boot_stick.h"
#include "../../shared/update/payload.h"
#include "../../shared/update/stick_set.h"
#if defined(BROOK_BRIDGE_V3)
#include "debug_bridge_v3.h"
#endif

namespace brook {
namespace {

KMutex g_lock{};
bool g_enabled = false;
uint32_t g_armed = 0;
int32_t g_lastRefusal = 0;
uint32_t g_runningLocation = 0; // where ARM last found the running pair
Device* g_partition = nullptr;
uint8_t g_pdrv = 0xff;
BootStick g_stick{};
bool g_stickPresent = false;

constexpr const char* MountPath = BROOK_BOOT_STICK_MOUNT;

int Refuse(int code, const char* why)
{
    g_lastRefusal = code;
    KPrintf("STICK: update refused (%s)\n", why);
    return -1; // EPERM
}

void Hex(char* out, const uint8_t* bytes, unsigned count)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < count; ++i) { out[2 * i] = digits[bytes[i] >> 4]; out[2 * i + 1] = digits[bytes[i] & 15]; }
    out[2 * count] = 0;
}

// Exactly one registered USB GPT partition must carry the loader's identity.
Device* FindPartition(int* code)
{
    Device* found = nullptr;
    unsigned matches = 0;
    for (unsigned disk = 0; disk < 10; ++disk) {
        for (unsigned part = 1; part <= 9; ++part) {
            char name[8] = {'u', 's', 'b', char('0' + disk), 'p', char('0' + part), 0};
            Device* dev = DeviceFind(name);
            if (!dev) continue;
            uint8_t guid[16]; uint32_t entry = 0; uint64_t first = 0, sectors = 0;
            if (!PartitionGptIdentity(dev, guid, &entry, &first, &sectors)) continue;
            if (entry != g_stick.partitionNumber || first != g_stick.firstLba || sectors != g_stick.sectors ||
                !BrookUpdateEqual(guid, g_stick.partitionGuid, 16)) continue;
            found = dev; ++matches;
        }
    }
    *code = matches == 1 ? 0 : matches ? BROOK_BOOT_STICK_REFUSED_AMBIGUOUS : BROOK_BOOT_STICK_REFUSED_NO_DEVICE;
    return matches == 1 ? found : nullptr;
}

bool LabelMatches(Device* dev)
{
    alignas(16) uint8_t sector[512];
    if (!dev->ops || !dev->ops->read || dev->ops->read(dev, 0, sector, 512) != 512) return false;
    static const uint8_t label[11] = {'B', 'R', 'O', 'O', 'K', '_', 'R', 'T', 'X', ' ', ' '};
    static const uint8_t type[8] = {'F', 'A', 'T', '3', '2', ' ', ' ', ' '};
    return sector[510] == 0x55 && sector[511] == 0xaa && BrookUpdateEqual(sector + 0x47, label, 11) &&
           BrookUpdateEqual(sector + 0x52, type, 8);
}

bool HashFile(const char* path, uint64_t expectedBytes, uint8_t out[32])
{
    Vnode* node = VfsOpen(path, VFS_O_STREAM);
    if (!node) return false;
    static uint8_t chunk[65536];
    BrookSha256 hash; BrookShaInit(&hash);
    uint64_t offset = 0, total = 0;
    bool ok = true;
    for (;;) {
        const int n = VfsRead(node, chunk, sizeof(chunk), &offset);
        if (n < 0) { ok = false; break; }
        if (!n) break;
        BrookShaAdd(&hash, chunk, static_cast<BrookUpdateSize>(n));
        total += static_cast<uint64_t>(n);
        if (total > expectedBytes) { ok = false; break; }
    }
    VfsClose(node);
    if (!ok || total != expectedBytes) return false;
    BrookShaFinish(&hash, out);
    return true;
}

// The pair the loader booted must be on the volume, byte for byte, as the
// root set or as PREV/ (after an update the running set lives in PREV/).
[[maybe_unused]] bool SetMatches(bool prev)
{
    char path[64];
    auto join = [&](const char* tail) {
        unsigned n = 0;
        for (const char* p = MountPath; *p; ++p) path[n++] = *p;
        for (const char* p = tail; *p && n < sizeof(path) - 1; ++p) path[n++] = *p;
        path[n] = 0;
        return path;
    };
    Vnode* record = VfsOpen(join(prev ? "/PREV/SET.BIN" : "/SET.BIN"));
    if (!record) return false;
    uint8_t set[BROOK_STICK_SET_BYTES + 1];
    uint64_t offset = 0;
    const int n = VfsRead(record, set, sizeof(set), &offset);
    VfsClose(record);
    if (n != int(BROOK_STICK_SET_BYTES) || !BrookStickSetValid(set) ||
        !BrookUpdateEqual(BrookStickSetKernelSha(set), g_stick.kernelSha256, 32) ||
        !BrookUpdateEqual(BrookStickSetInitrdSha(set), g_stick.initrdSha256, 32)) return false;
    uint8_t digest[32];
    if (!HashFile(join(prev ? "/PREV/BROOK.ELF" : "/KERNEL/BROOK.ELF"), BrookStickSetKernelBytes(set), digest) ||
        !BrookUpdateEqual(digest, g_stick.kernelSha256, 32)) return false;
    if (!HashFile(join(prev ? "/PREV/INITRD.IMG" : "/INITRD.IMG"), BrookStickSetInitrdBytes(set), digest) ||
        !BrookUpdateEqual(digest, g_stick.initrdSha256, 32)) return false;
    return true;
}

bool RunningPayloadMatches()
{
    if (g_stick.set != BROOK_STICK_ROOT && g_stick.set != BROOK_STICK_PREV) return false;
#if defined(BROOK_BRIDGE_V3)
    // Independent of the loader: the kernel hashed the initrd it was given.
    char initrdHex[65]; Hex(initrdHex, g_stick.initrdSha256, 32);
    const char* measured = DebugBridgeV3InitrdSha256();
    if (!measured) return false;
    for (unsigned i = 0; i < 65; ++i) if (measured[i] != initrdHex[i]) return false;
    const bool prev = g_stick.set == BROOK_STICK_PREV;
    if (SetMatches(prev)) { g_runningLocation = prev ? BROOK_STICK_PREV : BROOK_STICK_ROOT; return true; }
    if (SetMatches(!prev)) { g_runningLocation = prev ? BROOK_STICK_ROOT : BROOK_STICK_PREV; return true; }
    return false;
#else
    return false;
#endif
}

int SetWindow(bool open)
{
    Device* parent = PartitionParentDevice(g_partition);
    if (!parent || !parent->ops || !parent->ops->ioctl) return -5;
    DeviceWriteWindow window{};
    if (open) { window.start = g_stick.firstLba * 512; window.length = g_stick.sectors * 512; }
    return parent->ops->ioctl(parent, DEVICE_IOCTL_WRITE_WINDOW, &window);
}

uint8_t BoundDrive(Device* dev)
{
    for (uint8_t i = 0; i < FF_VOLUMES; ++i) if (FatFsBoundDevice(i) == dev) return i;
    for (uint8_t i = 1; i < FF_VOLUMES; ++i)
        if (!FatFsBoundDevice(i) && FatFsBindBootReadOnlyDrive(i, dev)) return i;
    return 0xff;
}

int DisarmLocked()
{
    if (!g_armed) return 0;
    int result = 0;
    if (g_armed == BROOK_BOOT_STICK_RW) {
        if (DeviceFlush(g_partition) != 0) result = -5;
        if (SetWindow(false) != 0) result = -5;
    }
    VfsUnmount(MountPath);
    if (g_armed == BROOK_BOOT_STICK_RW && !FatFsSetBootStickWritable(g_pdrv, g_partition, false)) {
        // The window is already closed, so no write can reach the device.
        KPuts("STICK: FAT binding still writable (mount busy); USB window closed\n");
        result = -16;
    }
    KPrintf("STICK: disarmed (%s)\n", result ? "with errors" : "clean");
    g_armed = 0;
    return result;
}

int ArmLocked(uint32_t mode)
{
    if (!g_enabled) return Refuse(BROOK_BOOT_STICK_REFUSED_DISABLED, "no /etc/boot-stick-update");
    if (!g_stickPresent || !g_stick.volumeValid)
        return Refuse(BROOK_BOOT_STICK_REFUSED_NO_LOADER_VOLUME, "loader reported no boot volume");
    if (g_armed) return Refuse(BROOK_BOOT_STICK_REFUSED_BUSY, "already armed");
    int code = 0;
    Device* part = FindPartition(&code);
    if (!part) return Refuse(code, code == BROOK_BOOT_STICK_REFUSED_AMBIGUOUS ?
                             "several partitions match the boot volume" : "boot volume not found on USB");
    if (!LabelMatches(part)) return Refuse(BROOK_BOOT_STICK_REFUSED_LABEL, "volume is not FAT32 labelled BROOK_RTX");
    const uint8_t pdrv = BoundDrive(part);
    if (pdrv == 0xff) return Refuse(BROOK_BOOT_STICK_REFUSED_MOUNT, "no FAT drive slot");
    g_partition = part; g_pdrv = pdrv;
    if (!VfsMountConfigured(MountPath, "fatfs", pdrv, MountReadOnly))
        return Refuse(BROOK_BOOT_STICK_REFUSED_MOUNT, "read-only mount failed");
    g_armed = BROOK_BOOT_STICK_RO;
    if (!RunningPayloadMatches()) {
        DisarmLocked();
        return Refuse(g_stick.set ? BROOK_BOOT_STICK_REFUSED_PAYLOAD : BROOK_BOOT_STICK_REFUSED_LEGACY_SET,
                      "running kernel/initrd differ from the booted set on the volume");
    }
    if (mode == BROOK_BOOT_STICK_RW) {
        VfsUnmount(MountPath);
        g_armed = 0;
        if (!FatFsSetBootStickWritable(pdrv, part, true))
            return Refuse(BROOK_BOOT_STICK_REFUSED_MOUNT, "FAT binding busy");
        if (SetWindow(true) != 0) {
            FatFsSetBootStickWritable(pdrv, part, false);
            return Refuse(BROOK_BOOT_STICK_REFUSED_MOUNT, "USB write window refused");
        }
        g_armed = BROOK_BOOT_STICK_RW;
        if (!VfsMountConfigured(MountPath, "fatfs", pdrv, 0)) {
            DisarmLocked();
            return Refuse(BROOK_BOOT_STICK_REFUSED_MOUNT, "writable mount failed");
        }
    }
    g_lastRefusal = 0;
    KPrintf("STICK: armed %s %s (set=%s)\n", mode == BROOK_BOOT_STICK_RW ? "read-write" : "read-only",
            part->name, g_stick.set == BROOK_STICK_PREV ? "PREV" : "root");
    return 0;
}

const char* SetName(uint32_t set) { return set == BROOK_STICK_ROOT ? "root" : set == BROOK_STICK_PREV ? "PREV" : "legacy"; }
const char* TrialName(uint32_t trial)
{
    switch (trial) {
    case BROOK_STICK_TRIAL_STARTED: return "trial-started";
    case BROOK_STICK_TRIAL_UNCONFIRMED: return "trial-unconfirmed(fallback)";
    case BROOK_STICK_TRIAL_MARK_FAILED: return "trial-mark-failed";
    default: return "confirmed";
    }
}

} // namespace

void BootStickConfigure()
{
    g_stickPresent = BootTimingHandoffStick(&g_stick);
    if (Vnode* marker = VfsOpen("/etc/boot-stick-update")) { VfsClose(marker); g_enabled = true; }
    if (!g_enabled) return;
    if (!g_stickPresent || !g_stick.volumeValid) {
        KPuts("STICK: loader reported no boot volume; remote stick updates disabled\n");
        return;
    }
    char guid[33]; Hex(guid, g_stick.partitionGuid, 16);
    KPrintf("STICK: boot volume GPT entry %u LBA %lu+%lu guid %s set=%s %s\n", g_stick.partitionNumber,
            static_cast<unsigned long>(g_stick.firstLba), static_cast<unsigned long>(g_stick.sectors), guid,
            SetName(g_stick.set), TrialName(g_stick.trial));
}

void BootStickStatusLine()
{
    if (!g_enabled) return;
    KPrintf("STICK set=%s %s volume=%s updates=%s armed=%u\n", SetName(g_stick.set), TrialName(g_stick.trial),
            g_stickPresent && g_stick.volumeValid ? "identified" : "unknown",
            g_stickPresent && g_stick.volumeValid ? "remote" : "disabled", g_armed);
}

int BootStickIoctl(uint32_t cmd, uint64_t arg, bool (*copyOut)(uint64_t, const void*, uint64_t))
{
    KMutexLock(&g_lock);
    int result = -25;
    if (cmd == BROOK_IOCTL_BOOT_STICK_ARM) {
        result = (arg == BROOK_BOOT_STICK_RO || arg == BROOK_BOOT_STICK_RW) ? ArmLocked(uint32_t(arg)) : -22;
    } else if (cmd == BROOK_IOCTL_BOOT_STICK_DISARM) {
        result = DisarmLocked();
    } else if (cmd == BROOK_IOCTL_BOOT_STICK_STATUS) {
        BrookBootStickStatus status{};
        status.version = 1; status.armed = g_armed; status.set = g_stick.set; status.trial = g_stick.trial;
        status.last_refusal = g_lastRefusal;
        status.running_location = g_runningLocation;
        __builtin_memcpy(status.partition_guid, g_stick.partitionGuid, 16);
        __builtin_memcpy(status.kernel_sha256, g_stick.kernelSha256, 32);
        __builtin_memcpy(status.initrd_sha256, g_stick.initrdSha256, 32);
        result = g_enabled ? (copyOut(arg, &status, sizeof(status)) ? 0 : -14) : -19;
    }
    KMutexUnlock(&g_lock);
    return result;
}

} // namespace brook
