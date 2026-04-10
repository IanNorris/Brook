// fatfs_glue.cpp — bridges FatFS's diskio interface to our Device registry.
//
// FatFS uses a "physical drive number" (pdrv) to identify storage.
// We map each pdrv slot to a registered block Device*.
// Call FatFsMount(pdrv, device) before mounting any FatFS volume on that drive.
//
// Compiled as C++ but exposes the C functions FatFS expects.

#include "fatfs_glue.h"
#include "ramdisk.h"
#include "device.h"
#include "serial.h"

// FatFS headers (C)
extern "C" {
#include "ff.h"
#include "diskio.h"
}

namespace brook {

static constexpr uint8_t FATFS_MAX_DRIVES = 4;
static Device* g_fatfsDrives[FATFS_MAX_DRIVES] = {};

bool FatFsBindDrive(uint8_t pdrv, Device* dev)
{
    if (pdrv >= FATFS_MAX_DRIVES || !dev) return false;
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
    if (pdrv >= brook::FATFS_MAX_DRIVES || !brook::g_fatfsDrives[pdrv])
        return STA_NOINIT | STA_NODISK;
    return 0; // drive present and initialised
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv >= brook::FATFS_MAX_DRIVES || !brook::g_fatfsDrives[pdrv])
        return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv >= brook::FATFS_MAX_DRIVES) return RES_PARERR;
    brook::Device* dev = brook::g_fatfsDrives[pdrv];
    if (!dev) return RES_NOTRDY;

    // FF_MIN_SS == FF_MAX_SS == 512, so block size is always 512.
    static constexpr uint32_t SECTOR_SIZE = 512;
    uint64_t offset = static_cast<uint64_t>(sector) * SECTOR_SIZE;
    uint64_t len    = static_cast<uint64_t>(count)  * SECTOR_SIZE;

    int ret = dev->ops->read(dev, offset, buff, len);
    return (ret == static_cast<int>(len)) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv >= brook::FATFS_MAX_DRIVES) return RES_PARERR;
    brook::Device* dev = brook::g_fatfsDrives[pdrv];
    if (!dev) return RES_NOTRDY;

    static constexpr uint32_t SECTOR_SIZE = 512;
    uint64_t offset = static_cast<uint64_t>(sector) * SECTOR_SIZE;
    uint64_t len    = static_cast<uint64_t>(count)  * SECTOR_SIZE;

    int ret = dev->ops->write(dev, offset, buff, len);
    return (ret == static_cast<int>(len)) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv >= brook::FATFS_MAX_DRIVES) return RES_PARERR;
    brook::Device* dev = brook::g_fatfsDrives[pdrv];
    if (!dev) return RES_NOTRDY;

    brook::SerialPrintf("disk_ioctl: pdrv=%u cmd=%u\n", (unsigned)pdrv, (unsigned)cmd);

    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_COUNT:
        {
            auto* p = static_cast<brook::RamdiskPriv*>(dev->priv);
            LBA_t cnt = p ? static_cast<LBA_t>(p->size / 512u) : 0;
            *static_cast<LBA_t*>(buff) = cnt;
        }
        return RES_OK;

    case GET_SECTOR_SIZE:
        *static_cast<WORD*>(buff) = 512;  // FF_MIN_SS == FF_MAX_SS == 512
        return RES_OK;

    case GET_BLOCK_SIZE:
        *static_cast<DWORD*>(buff) = 1; // erase block = 1 sector for ramdisk
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

} // extern "C"

// get_fattime() — required by FatFS when FF_FS_NORTC == 0.
// Returns a fixed timestamp (2026-01-01) until we have an RTC.
// Format: bits 31:25=year-1980, 24:21=month, 20:16=day, 15:11=hour, 10:5=min, 4:0=sec/2
extern "C" DWORD get_fattime(void)
{
    // 2026-01-01 00:00:00
    return ((2026u - 1980u) << 25) | (1u << 21) | (1u << 16);
}
