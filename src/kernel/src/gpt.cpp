#include "gpt.h"
#include "kprintf.h"
#include "serial.h"

// GPT on-disk structures (LBA 1 = primary header, LBA 2+ = entries).

namespace brook {

struct [[gnu::packed]] GptHeader {
    uint64_t signature;         // "EFI PART" = 0x5452415020494645
    uint32_t revision;
    uint32_t headerSize;
    uint32_t headerCrc32;
    uint32_t reserved;
    uint64_t myLba;
    uint64_t alternateLba;
    uint64_t firstUsableLba;
    uint64_t lastUsableLba;
    GptGuid  diskGuid;
    uint64_t partitionEntryLba;
    uint32_t numberOfPartEntries;
    uint32_t sizeOfPartEntry;
    uint32_t partEntryCrc32;
};

struct [[gnu::packed]] GptEntry {
    GptGuid  typeGuid;
    GptGuid  uniqueGuid;
    uint64_t startLba;
    uint64_t endLba;        // inclusive
    uint64_t attributes;
    uint16_t name[36];      // UTF-16LE
};

static_assert(sizeof(GptHeader) == 92, "GPT header size mismatch");
static_assert(sizeof(GptEntry) == 128, "GPT entry size mismatch");

static constexpr uint64_t GPT_SIGNATURE = 0x5452415020494645ULL; // "EFI PART"

// ---- Partition sub-device ----
// Each partition device stores a reference to the parent and an LBA offset/length.

struct PartitionPriv {
    Device*  parent;
    uint64_t startByte;
    uint64_t lengthBytes;
    char     name[16];   // "usb0p1", etc.
};

// We statically allocate up to GPT_MAX_PARTITIONS * MAX_DEVICES worth of
// partition descriptors.  For simplicity, a single flat pool.
static constexpr uint32_t PART_POOL_SIZE = 64;
static PartitionPriv s_partPool[PART_POOL_SIZE];
static Device        s_partDevices[PART_POOL_SIZE];
static uint32_t      s_partPoolNext = 0;

static int PartRead(Device* dev, uint64_t offset, void* buf, uint64_t len)
{
    auto* pp = static_cast<PartitionPriv*>(dev->priv);
    if (offset + len > pp->lengthBytes)
        len = pp->lengthBytes - offset;
    return pp->parent->ops->read(pp->parent, pp->startByte + offset, buf, len);
}

static int PartWrite(Device* dev, uint64_t offset, const void* buf, uint64_t len)
{
    auto* pp = static_cast<PartitionPriv*>(dev->priv);
    if (offset + len > pp->lengthBytes)
        len = pp->lengthBytes - offset;
    return pp->parent->ops->write(pp->parent, pp->startByte + offset, buf, len);
}

static uint64_t PartBlockCount(Device* dev)
{
    auto* pp = static_cast<PartitionPriv*>(dev->priv);
    return pp->lengthBytes / 512;
}

static uint32_t PartBlockSize(Device* /*dev*/)
{
    return 512;
}

static BlockDeviceOps s_partOps = {
    .read        = PartRead,
    .write       = PartWrite,
    .ioctl       = nullptr,
    .close       = nullptr,
    .block_count = PartBlockCount,
    .block_size  = PartBlockSize,
};

static bool GuidIsZero(const GptGuid& g)
{
    return g.data1 == 0 && g.data2 == 0 && g.data3 == 0 &&
           *(uint64_t*)g.data4 == 0;
}

static bool GuidEquals(const GptGuid& a, const GptGuid& b)
{
    return a.data1 == b.data1 && a.data2 == b.data2 && a.data3 == b.data3 &&
           *(uint64_t*)a.data4 == *(uint64_t*)b.data4;
}

uint32_t GptProbeDevice(Device* parentDev)
{
    if (!parentDev || !parentDev->ops || !parentDev->ops->read)
        return 0;

    // Read LBA 1 (GPT header).  Assume 512-byte sectors.
    alignas(16) uint8_t headerBuf[512];
    int n = parentDev->ops->read(parentDev, 512, headerBuf, 512);
    if (n < 512)
        return 0;

    auto* hdr = reinterpret_cast<GptHeader*>(headerBuf);
    if (hdr->signature != GPT_SIGNATURE)
        return 0;

    SerialPrintf("gpt: valid GPT on %s — %u entries\n",
                 parentDev->name, hdr->numberOfPartEntries);

    uint32_t entryCount = hdr->numberOfPartEntries;
    if (entryCount > GPT_MAX_PARTITIONS)
        entryCount = GPT_MAX_PARTITIONS;

    uint32_t entrySize = hdr->sizeOfPartEntry;
    if (entrySize < sizeof(GptEntry))
        entrySize = sizeof(GptEntry);

    // Read partition entries (starting at partitionEntryLba).
    // They may span multiple sectors.
    uint64_t entryBytes = static_cast<uint64_t>(entryCount) * entrySize;
    uint64_t entryOffset = hdr->partitionEntryLba * 512;

    // Read up to 16KB at a time (stack buffer).
    alignas(16) uint8_t entryBuf[16384];
    if (entryBytes > sizeof(entryBuf))
        entryBytes = sizeof(entryBuf);

    n = parentDev->ops->read(parentDev, entryOffset, entryBuf, entryBytes);
    if (n < static_cast<int>(entrySize))
        return 0;

    uint32_t registered = 0;
    uint32_t partIdx = 0;

    for (uint32_t i = 0; i < entryCount && s_partPoolNext < PART_POOL_SIZE; ++i)
    {
        auto* entry = reinterpret_cast<GptEntry*>(entryBuf + i * entrySize);

        // Skip empty entries.
        if (GuidIsZero(entry->typeGuid))
            continue;

        // Skip the EFI System Partition (bootloader-only, kernel doesn't need it).
        if (GuidEquals(entry->typeGuid, GPT_TYPE_EFI_SYSTEM))
            continue;

        partIdx++;

        uint64_t startByte = entry->startLba * 512;
        uint64_t endByte   = (entry->endLba + 1) * 512;

        // Allocate from pool.
        uint32_t slot = s_partPoolNext++;
        PartitionPriv& pp = s_partPool[slot];
        pp.parent      = parentDev;
        pp.startByte   = startByte;
        pp.lengthBytes = endByte - startByte;

        // Name: "<parent>p<N>"
        const char* pn = parentDev->name;
        uint32_t ni = 0;
        while (pn[ni] && ni < 10) { pp.name[ni] = pn[ni]; ni++; }
        pp.name[ni++] = 'p';
        if (partIdx >= 10) pp.name[ni++] = '0' + (partIdx / 10);
        pp.name[ni++] = '0' + (partIdx % 10);
        pp.name[ni] = '\0';

        Device& d = s_partDevices[slot];
        d.ops  = reinterpret_cast<const DeviceOps*>(&s_partOps);
        d.name = pp.name;
        d.type = DeviceType::Block;
        d.priv = &pp;

        if (DeviceRegister(&d))
        {
            SerialPrintf("gpt:   %s — LBA %lu..%lu (%lu MB)\n",
                         pp.name,
                         (unsigned long)entry->startLba,
                         (unsigned long)entry->endLba,
                         (unsigned long)((endByte - startByte) / (1024 * 1024)));
            registered++;
        }
    }

    return registered;
}

} // namespace brook
