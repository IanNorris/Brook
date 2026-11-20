#pragma once

// syscall_maintenance_block.h -- internal declarations shared from syscall_maintenance_block.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "device.h"

#include "spinlock.h"
#include "sync/kmutex.h"

namespace brook {

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)

// A maintenance-only descriptor for an unmounted, identity-checked target.
// Normal desktop builds do not expose it.
struct MaintenanceBlockFd {
    Device* device;
    uint64_t start,bytes,position;
    uint32_t partition;
    uint32_t refs;
    KMutex lock;
};

extern SpinLock maintenanceBlockLock;
extern uint32_t maintenanceBlockHolders, maintenanceBlockPublished;
extern uint64_t maintenanceBlockOwnerIncarnation, maintenanceBlockStart, maintenanceBlockBytes;
bool MaintenanceBlockReserve(Process* proc,uint32_t partition,bool& first,uint64_t& start,uint64_t& bytes);
void MaintenanceBlockRelease();
Device* MaintenanceBlockDevice();
bool MaintenanceBlockPath(const char* path,uint32_t& partition);
bool MaintenanceBlockRange(Device* device,uint32_t index,uint64_t& start,uint64_t& bytes);

int64_t MaintenanceBlockTransfer(MaintenanceBlockFd* file,uint64_t address,
                                 uint64_t count,uint64_t* position,
                                 bool write,bool userBuffer);
#endif

} // namespace brook
