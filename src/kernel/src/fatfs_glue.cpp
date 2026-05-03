// fatfs_glue.cpp — bridges FatFS's diskio interface to our Device registry.
//
// FatFS uses a "physical drive number" (pdrv) to identify storage.
// We map each pdrv slot to a registered block Device*.
// Call FatFsMount(pdrv, device) before mounting any FatFS volume on that drive.
//
// Compiled as C++ but exposes the C functions FatFS expects.

#include "fatfs_glue.h"
#include "ramdisk.h"
#include "rtc.h"
#include "device.h"
#include "serial.h"

// FatFS headers (C)
extern "C" {
#include "ff.h"
#include "diskio.h"
}

namespace brook {

static constexpr uint8_t FATFS_MAX_DRIVES = FF_VOLUMES;
static Device* g_fatfsDrives[FATFS_MAX_DRIVES] = {};

static bool KernelPtrLooksCanonical(const void* ptr)
{
    uint64_t v = reinterpret_cast<uint64_t>(ptr);
    return v != 0 && (v >> 47) == 0x1FFFFULL;
}

static bool FatFsDeviceReady(uint8_t pdrv, Device** out)
{
    if (out) *out = nullptr;
    if (pdrv >= FATFS_MAX_DRIVES) return false;

    Device* dev = g_fatfsDrives[pdrv];
    bool registered = DeviceIsRegistered(dev);
    const BlockDeviceOps* ops = (registered && KernelPtrLooksCanonical(dev->ops))
        ? reinterpret_cast<const BlockDeviceOps*>(dev->ops)
        : nullptr;
    if (!registered || dev->type != DeviceType::Block || !ops ||
        !ops->read || !ops->write || !ops->block_count || !ops->block_size)
    {
        static volatile uint32_t warnCount = 0;
        uint32_t n = __atomic_fetch_add(&warnCount, 1, __ATOMIC_RELAXED);
        if (n < 8 || (n & 0xFF) == 0)
        {
            SerialPrintf("FatFS: drive %u invalid dev=%p registered=%u ops=%p read=%p write=%p [#%u]\n",
                         pdrv, dev, registered ? 1u : 0u,
                         (registered && dev) ? dev->ops : nullptr,
                         ops ? ops->read : nullptr,
                         ops ? ops->write : nullptr,
                         n);
        }
        return false;
    }

    if (out) *out = dev;
    return true;
}

bool FatFsBindDrive(uint8_t pdrv, Device* dev)
{
    bool registered = DeviceIsRegistered(dev);
    const BlockDeviceOps* ops = (registered && KernelPtrLooksCanonical(dev->ops))
        ? reinterpret_cast<const BlockDeviceOps*>(dev->ops)
        : nullptr;
    if (pdrv >= FATFS_MAX_DRIVES || !registered ||
        dev->type != DeviceType::Block || !ops || !ops->read ||
        !ops->write || !ops->block_count || !ops->block_size)
    {
        SerialPrintf("FatFS: refusing invalid drive %u dev=%p\n", pdrv, dev);
        return false;
    }
    g_fatfsDrives[pdrv] = dev;
    SerialPrintf("FatFS: drive %u → '%s'\n", pdrv, dev->name);
    return true;
}

} // namespace brook

// ---------------------------------------------------------------------------
// FatFS diskio callbacks (must be C linkage)
// ---------------------------------------------------------------------------

extern "C" {

DSTATUS disk_status(BYTE pdrv)
{
    if (!brook::FatFsDeviceReady(pdrv, nullptr))
        return STA_NOINIT | STA_NODISK;
    return 0; // drive present and initialised
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (!brook::FatFsDeviceReady(pdrv, nullptr))
        return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    brook::Device* dev = nullptr;
    if (!brook::FatFsDeviceReady(pdrv, &dev)) return RES_NOTRDY;

    // FF_MIN_SS == FF_MAX_SS == 512, so block size is always 512.
    static constexpr uint32_t SECTOR_SIZE = 512;
    uint64_t offset = static_cast<uint64_t>(sector) * SECTOR_SIZE;
    uint64_t len    = static_cast<uint64_t>(count)  * SECTOR_SIZE;

    int ret = dev->ops->read(dev, offset, buff, len);
    return (ret == static_cast<int>(len)) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    brook::Device* dev = nullptr;
    if (!brook::FatFsDeviceReady(pdrv, &dev)) return RES_NOTRDY;

    static constexpr uint32_t SECTOR_SIZE = 512;
    uint64_t offset = static_cast<uint64_t>(sector) * SECTOR_SIZE;
    uint64_t len    = static_cast<uint64_t>(count)  * SECTOR_SIZE;

    int ret = dev->ops->write(dev, offset, buff, len);
    return (ret == static_cast<int>(len)) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    brook::Device* dev = nullptr;
    if (!brook::FatFsDeviceReady(pdrv, &dev)) return RES_NOTRDY;

    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_COUNT:
        {
            uint64_t sectors = brook::DeviceBlockCount(dev);
            *static_cast<LBA_t*>(buff) = static_cast<LBA_t>(sectors);
        }
        return RES_OK;

    case GET_SECTOR_SIZE:
        *static_cast<WORD*>(buff) = static_cast<WORD>(brook::DeviceBlockSize(dev));
        return RES_OK;

    case GET_BLOCK_SIZE:
        *static_cast<DWORD*>(buff) = 1; // erase block = 1 sector
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

} // extern "C"

// get_fattime() — required by FatFS when FF_FS_NORTC == 0.
// Returns current RTC time in packed FAT format.
// Format: bits 31:25=year-1980, 24:21=month, 20:16=day, 15:11=hour, 10:5=min, 4:0=sec/2
extern "C" DWORD get_fattime(void)
{
    uint64_t epoch = brook::RtcNow();
    if (epoch == 0) // RTC not yet initialized
        return ((2026u - 1980u) << 25) | (1u << 21) | (1u << 16);

    uint64_t rem = epoch;
    uint32_t sec = rem % 60; rem /= 60;
    uint32_t min = rem % 60; rem /= 60;
    uint32_t hr  = rem % 24; rem /= 24;
    uint64_t days = rem;
    uint32_t yr = 1970;
    while (true) {
        uint32_t diy = ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0) ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        yr++;
    }
    static const uint16_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t mon = 1;
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t dim = mdays[m];
        if (m == 1 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)) dim = 29;
        if (days < dim) { mon = m + 1; break; }
        days -= dim;
    }
    uint32_t day = static_cast<uint32_t>(days) + 1;
    return ((yr - 1980u) << 25) | (mon << 21) | (day << 16) |
           (hr << 11) | (min << 5) | (sec / 2);
}
