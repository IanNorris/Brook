// H-05 / H-06: ioctl and virtio DRM handlers copy their argument structs,
// and the GPU driver only ever sees kernel buffers.
#include "harness.h"
#include "syscall_ioctl.cpp"
#include "syscall_drm.cpp"
#include <stdlib.h>

using namespace harness;
namespace brook {
uint32_t SchedulerTranslateProcessGroup(const Process*, uint32_t group, bool) { return group; }
}
namespace {
bool g_capsetSawUserPointer;
brook::Vnode* g_espRoot;
brook::BootGuardSlot g_bootSlot{};
uint32_t g_bootFlags;
bool g_guardIdentityReady;
unsigned g_guardDigestCalls;
uint8_t g_guardIdentityDigest[32];
int32_t FakeGetCapset(uint32_t,uint32_t,void* out,uint32_t bytes) {
    if(InArena(reinterpret_cast<uint64_t>(out),1,Access::Read))g_capsetSawUserPointer=true;
    __builtin_memset(out,0x77,bytes<8?bytes:8);
    return 8;
}
brook::GpuAppCapabilities FakeCapabilities() { brook::GpuAppCapabilities caps{};caps.capsetMask=2;return caps; }
uint64_t Patterned(uint64_t offset,uint64_t length) {
    __builtin_memset(reinterpret_cast<void*>(UserAddress(offset)),0x5a,length);return UserAddress(offset);
}
bool Untouched(uint64_t address,uint64_t length) {
    for(uint64_t i=0;i<length;++i)if(reinterpret_cast<const uint8_t*>(address)[i]!=0x5a)return false;
    return true;
}
bool RaceSafe(int64_t result,uint64_t address,uint64_t length) {
    return !g_racedUnmaps || (result==-brook::EFAULT && Untouched(address,length));
}
}

namespace brook {
bool AhciSystemVolumeOwnsRoot(Vnode* node) { return node==g_espRoot; }
BootGuardSlot BootTimingHandoffSlot() { return g_bootSlot; }
uint32_t BootTimingHandoffFlags() { return g_bootFlags; }
int GuardedDesktopCheckGuardIdentity(const uint8_t (&digest)[32]) {
    ++g_guardDigestCalls;
    if(!g_guardIdentityReady)return -11;
    return BrookUpdateEqual(digest,g_guardIdentityDigest,sizeof(digest))?0:-116;
}
}

static void SystemEspBootSlot() {
    Reset();ResetProcess();
    brook::Vnode root{};
    root.type=brook::VnodeType::Dir;
    auto& fd=CurrentProcess()->fds[3];
    fd.type=brook::FdType::Vnode;
    fd.handle=&root;
    const auto output=Patterned(0,sizeof(BrookSystemEspBootSlot));
    const auto query=[](uint64_t address) {
        return brook::sys_ioctl(3,BROOK_IOCTL_GET_SYSTEM_ESP_BOOT_SLOT,address,0,0,0);
    };
    SetNamespaceAdmin(false);
    CHECK(query(output)==-brook::EPERM && Untouched(output,sizeof(BrookSystemEspBootSlot)));
    SetNamespaceAdmin(true);
    CHECK(query(output)==-brook::ENODEV && Untouched(output,sizeof(BrookSystemEspBootSlot)));
    g_espRoot=&root;
    g_bootSlot.generation=41;
    g_bootSlot.slot=1;
    g_bootSlot.selectorDigest[0]=0xa4;
    CHECK(query(g_kernel.Address())==-brook::EFAULT && g_kernel.Intact());
    CHECK(query(output)==0);
    const auto& slot=*reinterpret_cast<const BrookSystemEspBootSlot*>(output);
    CHECK(slot.version==BROOK_ESP_BOOT_SLOT_VERSION && slot.mode==BROOK_ESP_BOOT_SLOT_VERIFIED);
    CHECK(slot.generation==41 && slot.slot==1 && slot.selector_digest[0]==0xa4 && !slot.reserved);
    g_bootSlot={};
    g_bootFlags=brook::BootTimingRecovery;
    CHECK(query(output)==0 && slot.mode==BROOK_ESP_BOOT_SLOT_RECOVERY && !slot.generation);
    g_bootFlags=0;
    CHECK(query(output)==0 && slot.mode==BROOK_ESP_BOOT_SLOT_UNTRACKED && !slot.slot);
    g_espRoot=nullptr;
    SetNamespaceAdmin(false);
}

static void Termios() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    proc->fds[0].type=brook::FdType::DevKeyboard;proc->ttyCanonical=true;
    CHECK(brook::sys_ioctl(0,0x5401,g_kernel.Address(),0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_ioctl(0,0x5413,g_kernel.Address(),0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_ioctl(0,0x540F,g_kernel.Address(),0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_ioctl(0,0x5402,g_kernel.Address(),0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    const uint64_t t=UserAddress(0);
    CHECK(brook::sys_ioctl(0,0x5401,t,0,0,0)==0);
    const auto* flags=reinterpret_cast<const uint32_t*>(t);
    CHECK(flags[0]==0x0500 && flags[3]==(0x8A31|0x0002));
    CHECK(reinterpret_cast<const uint8_t*>(t)[17]==0x03 && reinterpret_cast<const uint8_t*>(t)[17+6]==1);
    // TCSETS clears ICANON.
    uint32_t set[4]={0,0,0,0x8A31};__builtin_memcpy(reinterpret_cast<void*>(UserAddress(64)),set,sizeof(set));
    CHECK(brook::sys_ioctl(0,0x5402,UserAddress(64),0,0,0)==0 && !proc->ttyCanonical);
    CHECK(brook::sys_ioctl(0,0x5413,UserAddress(128),0,0,0)==0);
    CHECK(reinterpret_cast<const uint16_t*>(UserAddress(128))[1]==80);
    // A raced unmap after validation never sees a raw store.
    Reset();ResetProcess();proc=CurrentProcess();proc->fds[0].type=brook::FdType::DevKeyboard;
    g_raceUnmapAfterCheck=true;
    const uint64_t raced=Patterned(0,36);
    CHECK(RaceSafe(brook::sys_ioctl(0,0x5401,raced,0,0,0),raced,36));
}

static void Fionread() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    auto* pipe=brook::PipeBufferCreate(64);pipe->readers=pipe->writers=1;
    CHECK(pipe->write("abc",3)==3);
    proc->fds[3].type=brook::FdType::Pipe;proc->fds[3].handle=pipe;proc->fds[3].flags=0;
    CHECK(brook::sys_ioctl(3,0x541B,UserAddress(0),0,0,0)==0);
    CHECK(*reinterpret_cast<const int*>(UserAddress(0))==3);
    g_raceUnmapAfterCheck=true;
    const uint64_t raced=Patterned(PageSize,4);
    CHECK(RaceSafe(brook::sys_ioctl(3,0x541B,raced,0,0,0),raced,4));
    // FIONBIO reads its argument with a checked copy.
    g_raceUnmapAfterCheck=false;
    CHECK(brook::sys_ioctl(3,0x5421,g_kernel.Address(),0,0,0)==-brook::EFAULT);
}

static void VirtioGetCaps() {
    Reset();ResetProcess();
    static brook::GpuAppOps ops{};ops.GetCapset=FakeGetCapset;ops.Capabilities=FakeCapabilities;
    g_gpuApp=&ops;g_capsetSawUserPointer=false;
    auto* context=static_cast<brook::DrmCtx*>(calloc(1,sizeof(brook::DrmCtx)));
    const uint64_t out=UserAddress(PageSize);
    __builtin_memset(reinterpret_cast<void*>(out),0x11,64);
    brook::DrmVirtgpuGetCaps request{1,1,out,64,0};
    __builtin_memcpy(reinterpret_cast<void*>(UserAddress(0)),&request,sizeof(request));
    CHECK(brook::DrmRenderIoctl(context,brook::DRM_NR_VIRTGPU_GET_CAPS,UserAddress(0))==0);
    // The driver filled a kernel buffer; the caller got its bytes and a zero tail.
    CHECK(!g_capsetSawUserPointer);
    CHECK(reinterpret_cast<const uint8_t*>(out)[7]==0x77 && reinterpret_cast<const uint8_t*>(out)[8]==0);
    // Kernel destination and kernel request pointers are refused.
    request.addr=g_kernel.Address();
    __builtin_memcpy(reinterpret_cast<void*>(UserAddress(0)),&request,sizeof(request));
    CHECK(brook::DrmRenderIoctl(context,brook::DRM_NR_VIRTGPU_GET_CAPS,UserAddress(0))==-brook::EFAULT);
    CHECK(brook::DrmRenderIoctl(context,brook::DRM_NR_GET_CAP,g_kernel.Address())==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    // GET_CAP's validate-then-read window.
    g_raceUnmapAfterCheck=true;
    const uint64_t cap=Patterned(2*PageSize,16);
    CHECK(RaceSafe(brook::DrmRenderIoctl(context,brook::DRM_NR_GET_CAP,cap),cap,16));
    g_gpuApp=nullptr;
    free(context);
}

static void SystemEspGuardIdentity() {
    Reset();ResetProcess();
    brook::Vnode root{};
    root.type=brook::VnodeType::Dir;
    auto& fd=CurrentProcess()->fds[3];
    fd.type=brook::FdType::Vnode;
    fd.handle=&root;
    const auto input=Patterned(0,sizeof(g_guardIdentityDigest));
    const auto query=[](uint64_t address) {
        return brook::sys_ioctl(3,BROOK_IOCTL_CHECK_SYSTEM_ESP_GUARD_IDENTITY,address,0,0,0);
    };
    SetNamespaceAdmin(false);
    g_espRoot=&root;
    CHECK(query(input)==-brook::EPERM && !g_guardDigestCalls);
    SetNamespaceAdmin(true);
    g_espRoot=nullptr;
    CHECK(query(input)==-brook::ENODEV && !g_guardDigestCalls);
    g_espRoot=&root;
    CHECK(query(g_kernel.Address())==-brook::EFAULT && !g_guardDigestCalls && g_kernel.Intact());
    CHECK(query(input)==-brook::EAGAIN && g_guardDigestCalls==1);
    g_guardIdentityReady=true;
    CHECK(query(input)==-116 && g_guardDigestCalls==2);
    __builtin_memset(g_guardIdentityDigest,0x5a,sizeof(g_guardIdentityDigest));
    CHECK(query(input)==0 && g_guardDigestCalls==3);
    CHECK(Untouched(input,sizeof(g_guardIdentityDigest)));
    g_guardIdentityReady=false;
    g_guardDigestCalls=0;
    g_espRoot=nullptr;
}

void RunIoctlTests() {
    Termios();
    Fionread();
    VirtioGetCaps();
    SystemEspBootSlot();
    SystemEspGuardIdentity();
}
