#include "test_framework.h"
#include "vfs.h"
#include "device.h"
#include "memory/heap.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "process.h"
#include "procfs.h"

// Scheduler stubs for KMutex — test_vfs is single-threaded so these are trivial.
static brook::Process g_stubProcess = {};
namespace brook {
    Process* SchedulerCurrentProcess() { return &g_stubProcess; }
    void SchedulerBlock(Process*) {}
    void SchedulerKernelRegionEnter(Process*) {}
    void SchedulerKernelRegionLeave(Process*) {}
    void SchedulerUnblock(Process*) {}

    // Procfs stubs — test_vfs doesn't test procfs.
    void ProcFsInit() {}
    Vnode* ProcFsOpen(const char*, int) { return nullptr; }
    int ProcFsStatPath(const char*, VnodeStat*) { return -1; }
    Vnode* ProcFsOpenDir(const char*) { return nullptr; }

    // RTC stub — return fixed epoch for get_fattime()
    uint64_t RtcNow() { return 1776556800ULL; } // 2026-04-15 approx
}

// A persistent-device model: writes enter a volatile cache; only a successful
// flush changes durable bytes. Exercise real VFS dispatch, error aggregation,
// retry and refusal to detach a mount whose final flush failed.
struct FlushFixture {
    brook::Device device;
    uint32_t pending, cached, durable, failures, syncCalls, unmountCalls;
};
static FlushFixture g_flush[2];
static int FixtureWrite(brook::Device* dev, uint64_t, const void* buf, uint64_t size)
{
    if (size != sizeof(uint32_t)) return -22;
    static_cast<FlushFixture*>(dev->priv)->cached = *static_cast<const uint32_t*>(buf);
    return sizeof(uint32_t);
}
static int FixtureIoctl(brook::Device* dev, uint32_t command, void*)
{
    if (command != brook::DEVICE_IOCTL_FLUSH) return -95;
    auto* f = static_cast<FlushFixture*>(dev->priv);
    if (f->failures) { --f->failures; return -5; }
    f->durable = f->cached;
    return 0;
}
static const brook::DeviceOps g_fixtureDeviceOps = {
    nullptr, FixtureWrite, FixtureIoctl, nullptr
};
static bool FixtureMount(uint8_t drive, void** out)
{
    if (drive >= 2) return false;
    auto* f = &g_flush[drive];
    *f = {};
    f->device = {&g_fixtureDeviceOps, "flush-fixture", brook::DeviceType::Block, f};
    *out = f;
    return true;
}
static int FixtureSync(void* priv)
{
    auto* f = static_cast<FlushFixture*>(priv);
    ++f->syncCalls;
    int result = f->device.ops->write(&f->device, 0, &f->pending, sizeof(f->pending));
    if (result != static_cast<int>(sizeof(f->pending))) return -5;
    return brook::DeviceFlush(&f->device);
}
static int FixtureUnmount(void* priv)
{
    auto* f = static_cast<FlushFixture*>(priv);
    ++f->unmountCalls;
    return FixtureSync(priv);
}
static int FixtureFileSync(brook::Vnode* vn) { return FixtureSync(vn->priv); }
static const brook::VnodeOps g_fixtureVnodeOps = {
    .open = nullptr, .read = nullptr, .write = nullptr,
    .readdir = nullptr, .close = nullptr, .stat = nullptr,
    .fsync = FixtureFileSync,
};
static const brook::VfsFsOps g_fixtureFsOps = {
    .mount = FixtureMount,
    .unmount = FixtureUnmount,
    .open = nullptr, .stat_path = nullptr, .lstat_path = nullptr,
    .unlink = nullptr, .mkdir = nullptr,
    .rename = nullptr, .symlink = nullptr, .readlink = nullptr,
    .sync = FixtureSync,
};

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

    brook::VfsRegisterFs("flush-fixture", &g_fixtureFsOps);
    ASSERT_TRUE(brook::VfsMount("/flush-a", "flush-fixture", 0));
    ASSERT_TRUE(brook::VfsMount("/flush-b", "flush-fixture", 1));
    g_flush[0].pending = 17;
    g_flush[1].pending = 29;
    g_flush[0].failures = 1;
    ASSERT_EQ(brook::VfsSync(nullptr), -5);
    ASSERT_EQ(g_flush[0].durable, 0u);
    ASSERT_EQ(g_flush[1].durable, 29u); // later mounts still synchronized
    ASSERT_EQ(brook::VfsSync(nullptr), 0);
    ASSERT_EQ(g_flush[0].durable, 17u);

    brook::Vnode file = {};
    file.ops = &g_fixtureVnodeOps;
    file.type = brook::VnodeType::File;
    file.priv = &g_flush[0];
    g_flush[0].pending = 41;
    g_flush[0].failures = 1;
    uint32_t otherSyncs = g_flush[1].syncCalls;
    ASSERT_EQ(brook::VfsFsync(&file), -5);
    ASSERT_EQ(g_flush[0].durable, 17u);
    ASSERT_EQ(brook::VfsSync(&file), 0);
    ASSERT_EQ(g_flush[0].durable, 41u);
    ASSERT_EQ(g_flush[1].syncCalls, otherSyncs); // per-file dispatch stays targeted

    g_flush[0].pending = 53;
    g_flush[0].failures = 1;
    ASSERT_FALSE(brook::VfsUnmount("/flush-a"));
    ASSERT_EQ(g_flush[0].durable, 41u);
    ASSERT_EQ(brook::VfsSync(nullptr), 0); // failed unmount retained the mount
    ASSERT_EQ(g_flush[0].durable, 53u);
    ASSERT_TRUE(brook::VfsUnmount("/flush-a"));
    ASSERT_EQ(g_flush[0].unmountCalls, 2u);
    ASSERT_TRUE(brook::VfsUnmount("/flush-b"));

    brook::DeviceOps unsupportedOps = {};
    brook::Device unsupported = {};
    unsupported.ops = &unsupportedOps;
    unsupported.name = "unknown";
    unsupported.type = brook::DeviceType::Block;
    ASSERT_EQ(brook::DeviceFlush(&unsupported), -95);

    brook::SerialPrintf("All VFS tests passed\n");
})
