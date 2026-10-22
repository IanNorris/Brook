#include "test_framework.h"
#include "vfs.h"
#include "vfs_path.h"
#include "fatfs_vfs.h"
#include "fatfs_glue.h"
#include "fat_test_image.h"
#include "ramdisk.h"
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

// Exercise real VFS mount ownership with a backend that has no busy callback.
// Its live directory name changes independently of the immutable opening label.
static unsigned g_retainedClosed, g_retainedUnmounted;
static bool g_retainedProbeClose, g_retainedBusyDuringClose, g_retainedFailOpen;
static bool RetainedMount(uint8_t,void** out) { *out=nullptr;return true; }
static int RetainedUnmount(void*) { ++g_retainedUnmounted;return 0; }
static int RetainedStat(void*,uint8_t,const char*,brook::VnodeStat* st) {
    *st={};st->isDir=true;st->mode=0040755;return 0;
}
static int RetainedCurrentName(brook::Vnode*,char* out,uint32_t size) {
    const char current[]="/moved";
    if(size<sizeof(current))return -36;
    for(unsigned i=0;i<sizeof(current);++i)out[i]=current[i];return 0;
}
static void RetainedClose(brook::Vnode*) {
    ++g_retainedClosed;
    // Only enabled for an explicit last-reference drop outside VFS locks.
    // Backend cleanup must finish before unmount is allowed to destroy state.
    if(g_retainedProbeClose) g_retainedBusyDuringClose=!brook::VfsUnmount("/pin");
}
static const brook::VnodeOps g_retainedNodeOps={
    .open=nullptr,.read=nullptr,.write=nullptr,.readdir=nullptr,
    .close=RetainedClose,.stat=nullptr,.fsync=nullptr,
    .directory_path=RetainedCurrentName,
};
static brook::Vnode* RetainedOpen(void*,uint8_t,const char*,int) {
    if(g_retainedFailOpen)return nullptr;
    auto* vn=static_cast<brook::Vnode*>(brook::kmalloc(sizeof(brook::Vnode)));
    if(!vn)return nullptr;
    *vn={};vn->ops=&g_retainedNodeOps;vn->type=brook::VnodeType::Dir;vn->refCount=1;return vn;
}
static const brook::VfsFsOps g_retainedFsOps={
    .mount=RetainedMount,.unmount=RetainedUnmount,.open=RetainedOpen,
    .stat_path=RetainedStat,.lstat_path=nullptr,.unlink=nullptr,.mkdir=nullptr,
    .rename=nullptr,.symlink=nullptr,.readlink=nullptr,.sync=nullptr,
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

    ASSERT_TRUE(brook::VfsRegisterFs("retained-fixture",&g_retainedFsOps));
    ASSERT_TRUE(brook::VfsMount("/","retained-fixture",0));
    ASSERT_TRUE(brook::VfsMount("/pin","retained-fixture",0));
    // Failed opens must not leave an orphan mount hold.
    g_retainedFailOpen=true;
    ASSERT_TRUE(brook::VfsOpen("/pin/failed")==nullptr);
    g_retainedFailOpen=false;
    ASSERT_TRUE(brook::VfsUnmount("/pin"));
    ASSERT_TRUE(brook::VfsMount("/pin","retained-fixture",0));
    g_retainedUnmounted=0;
    // An opened directory rejected as an executable or O_TRUNC must release
    // its already-attached mount hold through the ordinary error unwind.
    brook::Vnode* rejected=nullptr;
    char executableName[64];
    ASSERT_EQ(brook::VfsOpenExecutable("/pin/held",&rejected,executableName,sizeof(executableName)),-13);
    ASSERT_TRUE(rejected==nullptr);
    int openError=0;
    ASSERT_TRUE(brook::VfsOpenModeChecked("/pin/held",brook::VFS_O_TRUNC,0666,&openError)==nullptr);
    ASSERT_EQ(openError,-21);
    ASSERT_EQ(g_retainedClosed,2u);
    ASSERT_TRUE(brook::VfsUnmount("/pin"));
    ASSERT_TRUE(brook::VfsMount("/pin","retained-fixture",0));
    g_retainedClosed=g_retainedUnmounted=0;
    auto* retained=brook::VfsOpen("/pin/held");
    ASSERT_TRUE(retained!=nullptr);
    ASSERT_FALSE(brook::VfsUnmount("/pin"));
    ASSERT_EQ(g_retainedUnmounted,0u);
    // Cover the old bug: rediscovering the owner from /pin/held selected this
    // new overmount instead of the original mount containing the held inode.
    ASSERT_TRUE(brook::VfsMount("/pin/held","retained-fixture",1));
    char retainedName[64];
    ASSERT_EQ(brook::VfsDirectoryPath(retained,retainedName,sizeof(retainedName)),0);
    const char expectedName[]="/pin/moved";
    for(unsigned i=0;i<sizeof(expectedName);++i) ASSERT_EQ(retainedName[i],expectedName[i]);
    brook::VfsRetain(retained);
    brook::VfsRelease(retained);
    ASSERT_EQ(g_retainedClosed,0u);
    ASSERT_FALSE(brook::VfsUnmount("/pin"));
    g_retainedProbeClose=true;
    brook::VfsRelease(retained);
    g_retainedProbeClose=false;
    ASSERT_EQ(g_retainedClosed,1u);
    ASSERT_TRUE(g_retainedBusyDuringClose);
    ASSERT_TRUE(brook::VfsUnmount("/pin"));
    ASSERT_TRUE(brook::VfsUnmount("/pin/held"));
    ASSERT_TRUE(brook::VfsUnmount("/"));
    ASSERT_EQ(g_retainedUnmounted,3u);

    brook::DeviceOps unsupportedOps = {};
    brook::Device unsupported = {};
    unsupported.ops = &unsupportedOps;
    unsupported.name = "unknown";
    unsupported.type = brook::DeviceType::Block;
    ASSERT_EQ(brook::DeviceFlush(&unsupported), -95);

    // Native real-backend coverage: kernel heap/VMM, real KMutex/KRwLock,
    // FatFs disk glue, and retained object/attachment lifetimes together.
    brook::FatFsVfsRegister();
    auto* rootBytes=static_cast<uint8_t*>(brook::kmalloc(g_fatTestImageSize));
    auto* childBytes=static_cast<uint8_t*>(brook::kmalloc(g_fatTestImageSize));
    ASSERT_TRUE(rootBytes && childBytes);
    for(uint32_t i=0;i<g_fatTestImageSize;++i)rootBytes[i]=childBytes[i]=g_fatTestImage[i];
    auto* rootDevice=brook::RamdiskCreate(rootBytes,g_fatTestImageSize,512,"vfs-root");
    auto* childDevice=brook::RamdiskCreate(childBytes,g_fatTestImageSize,512,"vfs-child");
    ASSERT_TRUE(rootDevice && childDevice);
    ASSERT_TRUE(brook::DeviceRegister(rootDevice));ASSERT_TRUE(brook::DeviceRegister(childDevice));
    ASSERT_TRUE(brook::FatFsBindDrive(0,rootDevice));ASSERT_TRUE(brook::FatFsBindDrive(1,childDevice));
    ASSERT_TRUE(brook::VfsMount("/","fatfs",0));
    brook::VfsPath rootPath{};brook::VfsPath cwdPath{};brook::VfsPath filePath{};brook::VfsPath aliasPath{};
    ASSERT_EQ(brook::VfsPathRoot(&rootPath),0);
    ASSERT_EQ(brook::VfsPathResolve(rootPath,rootPath,"/TEST",&cwdPath),0);
    ASSERT_EQ(brook::VfsPathResolve(rootPath,cwdPath,"HELLO.TXT",&filePath),0);
    ASSERT_EQ(brook::VfsRename("/TEST","/MOVED"),0);
    ASSERT_EQ(brook::VfsMkdir("/TEST",0755),0);
    ASSERT_EQ(brook::VfsPathResolve(rootPath,cwdPath,"HELLO.TXT",&aliasPath),0);
    ASSERT_TRUE(brook::VfsPathSame(aliasPath,filePath));brook::VfsPathRelease(aliasPath);
    ASSERT_TRUE(brook::VfsMount("/MOVED","fatfs",1));
    ASSERT_EQ(brook::VfsPathResolve(rootPath,rootPath,"/MOVED/HELLO.TXT",&aliasPath),-2);
    ASSERT_EQ(brook::VfsPathResolve(rootPath,cwdPath,"HELLO.TXT",&aliasPath),0);
    ASSERT_TRUE(brook::VfsPathSame(aliasPath,filePath));brook::VfsPathRelease(aliasPath);
    ASSERT_TRUE(brook::VfsUnmount("/MOVED"));
    int retainedError=0;auto* exact=brook::VfsPathOpen(filePath,brook::VFS_O_READ,&retainedError);
    ASSERT_TRUE(exact!=nullptr);ASSERT_EQ(retainedError,0);
    char hello[32]{};uint64_t helloOffset=0;
    ASSERT_EQ(brook::VfsRead(exact,hello,19,&helloOffset),19);
    const char expectedHello[]="Hello from the VFS!\n";
    for(unsigned i=0;i<19;++i)ASSERT_EQ(hello[i],expectedHello[i]);
    brook::VfsPathRelease(filePath);brook::VfsPathRelease(cwdPath);brook::VfsPathRelease(rootPath);
    ASSERT_FALSE(brook::VfsUnmount("/")); // Only exact vnode still owns backing.
    brook::VfsClose(exact);ASSERT_TRUE(brook::VfsUnmount("/"));
    ASSERT_TRUE(brook::DeviceUnregister(childDevice));ASSERT_TRUE(brook::DeviceUnregister(rootDevice));
    brook::kfree(childDevice->priv);brook::kfree(childDevice);brook::kfree(rootDevice->priv);brook::kfree(rootDevice);
    brook::kfree(childBytes);brook::kfree(rootBytes);

    brook::SerialPrintf("All VFS tests passed\n");
})
