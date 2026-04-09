// virtio_blk_mod.cpp — virtio-blk driver module for Brook OS.
//
// This module wraps the kernel's built-in virtio-blk hardware driver
// (VirtioBlkInitAll, exported via ksymtab) and owns the disk-probing
// and VFS mount logic.
//
// Bootstrap note: the kernel's VirtioBlkInitAll() is called directly at
// startup to mount /boot (bootstrap necessity).  This module is loaded
// AFTER /boot is accessible and demonstrates that the module pipeline works.
// Once an initrd mechanism exists, VirtioBlkInitAll() can be moved entirely
// into this module and removed from the kernel binary.

#include "module_abi.h"
#include "device.h"
#include "fatfs_glue.h"
#include "vfs.h"
#include "serial.h"
#include "kprintf.h"

MODULE_IMPORT_SYMBOL(VirtioBlkInitAll);
MODULE_IMPORT_SYMBOL(DeviceFind);
MODULE_IMPORT_SYMBOL(FatFsBindDrive);
MODULE_IMPORT_SYMBOL(VfsMount);
MODULE_IMPORT_SYMBOL(VfsUnmount);
MODULE_IMPORT_SYMBOL(VfsOpen);
MODULE_IMPORT_SYMBOL(VfsRead);
MODULE_IMPORT_SYMBOL(VfsClose);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(SerialPrintf);

using namespace brook;

static int VirtioBlkModuleInit()
{
    SerialPuts("virtio_blk module: init\n");

    // Count how many virtio devices the kernel registered.
    uint32_t count = 0;
    for (;;)
    {
        char name[16] = "virtio";
        // Append digit — supports up to 8 devices.
        name[6] = static_cast<char>('0' + count);
        name[7] = '\0';
        if (!DeviceFind(name)) break;
        ++count;
    }

    KPrintf("virtio_blk module: %u virtio device(s) registered by kernel\n", count);

    // Report sector counts for each registered virtio device.
    for (uint32_t i = 0; i < count; ++i)
    {
        char name[16] = "virtio";
        name[6] = static_cast<char>('0' + i);
        name[7] = '\0';
        Device* dev = DeviceFind(name);
        if (!dev) continue;
        uint64_t sectors = DeviceBlockCount(dev);
        KPrintf("  %s: %lu sectors (%lu MB)\n",
                name, sectors, (sectors * 512) / (1024*1024));
    }

    return 0;
}

static void VirtioBlkModuleExit()
{
    SerialPuts("virtio_blk module: exit\n");
}

DECLARE_MODULE("virtio_blk", VirtioBlkModuleInit, VirtioBlkModuleExit,
               "virtio legacy block device driver (hardware init via kernel)");
