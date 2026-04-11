#include "test_framework.h"
#include "vfs.h"
#include "device.h"
#include "memory/heap.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "process.h"

// Scheduler stubs for KMutex — test_vfs is single-threaded so these are trivial.
static brook::Process g_stubProcess = {};
namespace brook {
    Process* SchedulerCurrentProcess() { return &g_stubProcess; }
    void SchedulerBlock(Process*) {}
    void SchedulerUnblock(Process*) {}
}

TEST_MAIN("vfs", {

    // Initialise memory subsystem (VFS needs kmalloc for Vnode allocation)
    brook::PmmInit(bp);
    brook::VmmInit();
    brook::HeapInit();
    brook::PmmEnableTracking();

    // 1. VfsInit succeeds
    brook::VfsInit();

    // 2. Open on path with no mount returns nullptr
    brook::Vnode* vn = brook::VfsOpen("/nonexistent");
    ASSERT_TRUE(vn == nullptr);

    // 3. Open with empty path returns nullptr
    vn = brook::VfsOpen("");
    ASSERT_TRUE(vn == nullptr);

    // 4. Open with nullptr doesn't crash
    vn = brook::VfsOpen(nullptr);
    ASSERT_TRUE(vn == nullptr);

    // 5. VfsClose on nullptr doesn't crash
    brook::VfsClose(nullptr);

    // 6. VfsRead with nullptr vnode returns error
    uint8_t buf[64];
    uint64_t off = 0;
    ASSERT_TRUE(brook::VfsRead(nullptr, buf, sizeof(buf), &off) < 0);

    // 7. VfsWrite with nullptr vnode returns error
    ASSERT_TRUE(brook::VfsWrite(nullptr, buf, sizeof(buf), &off) < 0);

    // 8. VfsReaddir with nullptr returns error
    brook::DirEntry entry;
    uint32_t cookie = 0;
    ASSERT_TRUE(brook::VfsReaddir(nullptr, &entry, &cookie) < 0);

    // 9. Mount with invalid fsName fails gracefully
    bool mounted = brook::VfsMount("/test", "unknown_fs", 0);
    ASSERT_FALSE(mounted);

    // 10. Unmount non-existent path returns false
    ASSERT_FALSE(brook::VfsUnmount("/not_mounted"));

    // 11. Mount fatfs at unbound drive — should fail at f_mount
    mounted = brook::VfsMount("/probe", "fatfs", 7);
    if (mounted)
        brook::VfsUnmount("/probe");

    brook::SerialPrintf("All VFS tests passed\n");
})
