#include "live_store.h"
#include "live_store_record.h"
#include "boot_stick.h"
#include "device.h"
#include "gpt.h"
#include "kprintf.h"
#include "ramdisk.h"
#include "apic.h"
#include "vfs.h"
#include "writable_desktop.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "../../shared/update/payload.h"

namespace brook {
namespace {
constexpr uint64_t ChunkBytes = 4ull << 20;
constexpr uint64_t RamHeadroom = 4ull << 30; // keep this much free after the copy

bool ReadRecord(LiveStoreRecord& record, bool quiet)
{
    Vnode* node = VfsOpen("/etc/live-store", VFS_O_STREAM);
    if (!node) return false;
    char text[200]{}; uint64_t offset = 0;
    const int n = VfsRead(node, text, sizeof(text) - 1, &offset);
    VfsClose(node);
    if (n <= 0 || !LiveStoreParse(text, uint32_t(n), record)) {
        if (!quiet) KPuts("live store: invalid /etc/live-store\n");
        return false;
    }
    return true;
}
}

bool LiveStoreIsStorePartition(Device* partition)
{
    LiveStoreRecord record{};
    if (!partition || !ReadRecord(record, true)) return false;
    uint8_t guid[16]; uint32_t entry = 0; uint64_t first = 0, sectors = 0;
    if (!PartitionGptIdentity(partition, guid, &entry, &first, &sectors)) return false;
    for (unsigned i = 0; i < 16; ++i) if (guid[i] != record.guid[i]) return false;
    return true;
}

bool LiveStoreBoot(uint8_t* nextDrive)
{
    LiveStoreRecord record{};
    if (!ReadRecord(record, false)) return false;
    const int disk = BootStickDiskIndex();
    if (disk < 0 || disk > 9) { KPuts("live store: boot stick disk not identified; kconsole session\n"); return false; }
    Device* part = nullptr;
    for (unsigned p = 1; p <= 9 && !part; ++p) {
        char name[8] = {'u', 's', 'b', char('0' + disk), 'p', char('0' + p), 0};
        Device* dev = DeviceFind(name);
        if (!dev) continue;
        uint8_t guid[16]; uint32_t entry = 0; uint64_t first = 0, sectors = 0;
        if (!PartitionGptIdentity(dev, guid, &entry, &first, &sectors)) continue;
        bool same = true; for (unsigned i = 0; i < 16; ++i) same &= guid[i] == record.guid[i];
        if (!same) continue;
        if (sectors * 512 < record.bytes) { KPrintf("live store: %s smaller than the record\n", name); return false; }
        part = dev;
    }
    if (!part) { KPuts("live store: store partition absent on the boot stick; kconsole session\n"); return false; }
    const uint64_t pages = record.bytes / 4096;
    if (PmmGetFreePageCount() < pages + RamHeadroom / 4096) { KPuts("live store: not enough RAM; kconsole session\n"); return false; }
    auto base = VmmAllocPages(pages, VMM_WRITABLE | VMM_NO_EXEC, MemTag::Device, KernelPid);
    if (!base) { KPuts("live store: allocation failed; kconsole session\n"); return false; }
    auto* memory = reinterpret_cast<uint8_t*>(base.raw());
    KPrintf("live store: reading %s %lu MiB into RAM\n", part->name, static_cast<unsigned long>(record.bytes >> 20));
    BrookSha256 hash; BrookShaInit(&hash);
    const uint64_t start = ApicTickCount();
    uint64_t lastReport = 0;
    for (uint64_t at = 0; at < record.bytes; at += ChunkBytes) {
        const uint64_t len = record.bytes - at < ChunkBytes ? record.bytes - at : ChunkBytes;
        const int got = part->ops->read(part, at, memory + at, len);
        if (got != int(len)) {
            KPrintf("live store: read failed at %lu (%d); kconsole session\n", static_cast<unsigned long>(at), got);
            VmmFreePages(base, pages); return false;
        }
        BrookShaAdd(&hash, memory + at, static_cast<BrookUpdateSize>(len));
        if (at + len - lastReport >= (512ull << 20) || at + len == record.bytes) {
            lastReport = at + len;
            const uint64_t ms = ApicTickCount() - start;
            KPrintf("live store: %lu/%lu MiB, %lu ms (%lu MB/s)\n", static_cast<unsigned long>(lastReport >> 20),
                    static_cast<unsigned long>(record.bytes >> 20), static_cast<unsigned long>(ms),
                    static_cast<unsigned long>(ms ? lastReport / 1000 / ms : 0));
        }
    }
    uint8_t digest[32]; BrookShaFinish(&hash, digest);
    bool match = true; for (unsigned i = 0; i < 32; ++i) match &= digest[i] == record.sha256[i];
    if (!match) { KPuts("live store: SHA-256 mismatch; kconsole session\n"); VmmFreePages(base, pages); return false; }
    Device* ram = RamdiskCreate(memory, record.bytes, 512, "live-store");
    if (!ram) { VmmFreePages(base, pages); return false; }
    const auto result = WritableDesktopLiveMount(nextDrive, ram, record.bytes);
    if (result != WritableDesktopBoot::Ready) { KPuts("live store: mount failed; kconsole session\n"); return false; }
    KPrintf("live store: /nix ready from RAM (%lu MiB, not persistent)\n", static_cast<unsigned long>(record.bytes >> 20));
    return true;
}
}
