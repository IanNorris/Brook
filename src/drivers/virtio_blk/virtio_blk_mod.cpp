// virtio_blk_mod.cpp — virtio-blk driver module for Brook OS.
//
// Bootstrap note: the kernel calls VirtioBlkInitAll() directly at startup to
// mount /boot (chicken-and-egg: modules live on /boot).  This module is loaded
// AFTER /boot is accessible.  It currently owns device enumeration/reporting
// and provides proper cleanup on unload.
//
// Once an initrd mechanism exists, the full hardware init can move here.

#include "module_abi.h"
#include "device.h"
#include "fatfs_glue.h"
#include "vfs.h"
#include "serial.h"
#include "kprintf.h"

MODULE_IMPORT_SYMBOL(VirtioBlkInitAll);
MODULE_IMPORT_SYMBOL(DeviceFind);
MODULE_IMPORT_SYMBOL(DeviceUnregister);
MODULE_IMPORT_SYMBOL(FatFsBindDrive);
MODULE_IMPORT_SYMBOL(VfsMount);
MODULE_IMPORT_SYMBOL(VfsUnmount);
MODULE_IMPORT_SYMBOL(VfsOpen);
MODULE_IMPORT_SYMBOL(VfsRead);
MODULE_IMPORT_SYMBOL(VfsClose);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(SerialPrintf);

using namespace brook;

static constexpr uint32_t MAX_VIRTIO_DEVS = 8;
static Device* g_ownedDevices[MAX_VIRTIO_DEVS];
static uint32_t g_ownedCount = 0;

static int VirtioBlkModuleInit()
{
    SerialPuts("virtio_blk module: init\n");

    // Count how many virtio devices the kernel registered during bootstrap.
    g_ownedCount = 0;
    for (uint32_t i = 0; i < MAX_VIRTIO_DEVS; ++i)
    {
        char name[16] = "virtio";
        name[6] = static_cast<char>('0' + i);
        name[7] = '\0';
        Device* dev = DeviceFind(name);
        if (!dev) break;
        g_ownedDevices[g_ownedCount++] = dev;
    }

    KPrintf("virtio_blk module: %u virtio device(s) registered by kernel\n", g_ownedCount);

    // Report sector counts for each registered virtio device.
    for (uint32_t i = 0; i < g_ownedCount; ++i)
    {
        Device* dev = g_ownedDevices[i];
        uint64_t sectors = DeviceBlockCount(dev);
        KPrintf("  %s: %lu sectors (%lu MB)\n",
                dev->name, sectors, (sectors * 512) / (1024*1024));
    }

    return 0;
}

static void VirtioBlkModuleExit()
{
    SerialPuts("virtio_blk module: exit — unregistering devices\n");
    for (uint32_t i = 0; i < g_ownedCount; ++i)
    {
        if (g_ownedDevices[i])
        {
            SerialPrintf("virtio_blk module: unregister %s\n", g_ownedDevices[i]->name);
            DeviceUnregister(g_ownedDevices[i]);
            g_ownedDevices[i] = nullptr;
        }
    }
    g_ownedCount = 0;
}

DECLARE_MODULE("virtio_blk", VirtioBlkModuleInit, VirtioBlkModuleExit,
               "virtio legacy block device driver (hardware init via kernel)");
