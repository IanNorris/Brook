// RTX 2080 display step 3: the gated live path for nv_disp_runner.h.
// `bridge hardware nv-disp plan|preflight|noop|flip|continue|abort|status`.
// Off by default: nothing here runs at boot, and the only GPU accesses are
// made from these explicit bridge commands (and the pre-reset quiesce hook,
// which only acts while a run is live). BAR0 registers and the BAR1 VRAM
// pages named by the plan are mapped uncached on demand; the run holds the
// nv-aux lock (serialised with nv-aux and dp-idle) and busy-waits on the TSC.
#include "nv_disp_live.h"
#include "nv_disp_runner.h"
#include "nv_disp_head_runner.h"
#include "nv_disp_driver.h"
#include "pci.h"
#include "acpi.h"
#include "boot_resource_map.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "net.h"
#include "kprintf.h"
#include "tsc_clock.h"
#include "platform_power.h"
#include "string.h"
#include "compositor.h"
#include "display.h"
#include "display_output.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include <serial.h>

inline void* operator new(size_t, void* storage) noexcept { return storage; }

namespace brook {
namespace {

// Small caches of single-page UC mappings (BAR0 registers, BAR1 VRAM pages).
// Pinned caches never remap a slot: the resident driver's flips run on the compositor
// thread, and a remapped slot would leave a stale TLB entry on another CPU (invlpg is
// local). Registers and the driver's VRAM region (instmem, pushbuffers, sync) use pinned
// caches; only bulk surface fills (bridge/driver thread) use the evicting one.
template<uint32_t Slots,bool Evict> struct PageCacheT{
    static constexpr uint32_t kSlots=Slots;
    VirtualAddress va[kSlots]{};uint64_t phys[kSlots]{};uint32_t next=0;
    volatile uint32_t* Map(uint64_t address){
        const uint64_t page=address&~0xfffull;
        for(uint32_t i=0;i<kSlots;++i)if(va[i]&&phys[i]==page)return reinterpret_cast<volatile uint32_t*>(va[i].raw()+(address&0xfff));
        if(!Evict&&next>=kSlots)return nullptr;          // pinned cache full: refuse rather than remap
        const uint32_t s=next++%kSlots;
        if(!va[s]){
            va[s]=VmmAllocPages(1,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
            if(!va[s])return nullptr;
            const auto leaf=VmmDetachPage(KernelPageTable,va[s]);if(leaf)PmmFreePage(VmmLeafPhysical(leaf));
        }else{(void)VmmDetachPage(KernelPageTable,va[s]);}
        __asm__ volatile("invlpg (%0)"::"r"(va[s].raw()):"memory");
        phys[s]=~0ull;
        if(!VmmMapPage(KernelPageTable,va[s],PhysicalAddress(page),VMM_WRITABLE|VMM_NO_EXEC|VMM_CACHE_DISABLE|VMM_WRITE_THRU,MemTag::Device,KernelPid))
            return nullptr;
        phys[s]=page;
        return reinterpret_cast<volatile uint32_t*>(va[s].raw()+(address&0xfff));
    }
};
using PinnedRegs=PageCacheT<192,false>;   // BAR0: registers, PRAMIN, the PROM (VBIOS read)
using PinnedVram=PageCacheT<64,false>;
using PageCache=PageCacheT<24,true>;

struct LiveBackend{
    uint64_t bar0=0,bar1=0,bar1Size=0;bool ok=false,toSerial=false;
    PinnedRegs regs{};PinnedVram vramTop{};PageCache vram{};
    // The top 1 MiB of the aperture (instmem, pushbuffers, sync page) is pinned.
    volatile uint32_t* VramMap(uint64_t a){return a+(1ull<<20)>=bar1Size?vramTop.Map(bar1+a):vram.Map(bar1+a);}
    uint32_t rd(uint32_t a){volatile uint32_t* p=regs.Map(bar0+a);return p?*p:0xffffffffu;}
    void wr(uint32_t a,uint32_t v){volatile uint32_t* p=regs.Map(bar0+a);if(p)*p=v;}
    uint32_t vrd(uint64_t a){if(a+4>bar1Size)return 0xffffffffu;volatile uint32_t* p=VramMap(a);return p?*p:0xffffffffu;}
    void vwr(uint64_t a,uint32_t v){if(a+4>bar1Size)return;volatile uint32_t* p=VramMap(a);if(p)*p=v;}
    void fill(uint64_t v,uint32_t bytes,uint32_t pitch,uint32_t colour,uint32_t marker,uint32_t lines){
        if(v+bytes>bar1Size||!pitch)return;
        const uint64_t markerEnd=uint64_t(lines)*pitch;
        for(uint64_t off=0;off<bytes;off+=4096){
            volatile uint32_t* p=vram.Map(bar1+v+off);if(!p)return;
            for(uint32_t i=0;i<1024;++i)p[i]=off+i*4<markerEnd?marker:colour;
        }
    }
    void fence(){__asm__ volatile("sfence":::"memory");}
    uint64_t nowUs(){return TscClockAvailable()?TscClockReadNs()/1000:ApicTickCount()*1000;}
    void relax(){__asm__ volatile("pause");}
    void emit(const char* line){if(toSerial)SerialPuts(line);else DebugChannelSend(line);}
    void hookQuiesce();
    bool step3Busy();
    bool step3Succeeded();
    void dpIdleSuspend(bool s){DpIdleSuspendForDisplay(s);}
};

[[clang::require_constant_initialization]] LiveBackend g_backend{};
// The runners are ~160 KiB each with non-zero member defaults; as constant-
// initialised globals they sat in .data and pushed the Dell kernel ELF past
// the installed loader's 8 MiB BROOK_UPDATE_KERNEL_MAX. Keep their storage in
// .bss and construct them on first use (an nv-disp command, under the AUX lock).
using RunnerT=nvdisp::Runner<LiveBackend>;
using HeadT=nvdisp::HeadRunner<LiveBackend>;
// Resident driver (D1): bridge-driven for now (`nv-disp driver ...`); D2/D3 wire it to the
// compositor and the desktop-start markers. Same .bss + lazy construction as the runners.
using DriverT=nvdisp::NvDisplayDriver<LiveBackend>;
alignas(DriverT) uint8_t g_driverStorage[sizeof(DriverT)];
alignas(RunnerT) uint8_t g_runnerStorage[sizeof(RunnerT)];
alignas(HeadT) uint8_t g_headStorage[sizeof(HeadT)];
bool g_runnersBuilt=false;
void EnsureRunners(){
    if(g_runnersBuilt)return;
    new(g_runnerStorage) RunnerT{};
    new(g_headStorage) HeadT{};
    new(g_driverStorage) DriverT{};
    g_runnersBuilt=true;
}
#define g_runner (*reinterpret_cast<RunnerT*>(g_runnerStorage))
#define g_head (*reinterpret_cast<HeadT*>(g_headStorage))
#define g_driver (*reinterpret_cast<DriverT*>(g_driverStorage))
bool g_configured=false;
uint8_t g_vbios[128*1024];bool g_vbiosRead=false;
constexpr uint32_t kVbiosHash=0xbb33ca0a; // FNV-1a of the first 128 KiB of PROM (offline dump)
bool LiveBackend::step3Busy(){return g_runnersBuilt&&(g_runner.st.live||g_runner.st.paused);}
bool LiveBackend::step3Succeeded(){return g_runnersBuilt&&g_runner.st.done>0;}

void NvDispQuiesceForReset(){
    // Pre-reset: abort a live run (the runner does nothing otherwise). Try the
    // lock briefly; a command stuck mid-run must not block the reset.
    SpinLock* lock=NvAuxSharedLock();bool held=false;
    const uint64_t end=g_backend.nowUs()+100000;
    while(!(held=SpinLockTryAcquire(lock))&&g_backend.nowUs()<end)__asm__ volatile("pause");
    if(!g_runnersBuilt){if(held)SpinLockRelease(lock);return;}
    g_backend.toSerial=true;
    if(g_driver.b)g_driver.Quiesce();   // flipped outputs back to the GOP state first
    g_runner.Quiesce();
    g_head.Quiesce();
    g_backend.toSerial=false;
    if(held)SpinLockRelease(lock);
}
extern "C" void NvDisplayMarkWritten();
void LiveBackend::hookQuiesce(){NvDisplayMarkWritten();(void)PlatformPowerAddQuiesce(NvDispQuiesceForReset);}

bool FindGpu(PciDevice& gpu){
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry e{};
        if(PciBootInventoryRead(i,&e)&&e.device.vendorId==0x10de&&e.device.classCode==0x03&&e.device.fn==0){gpu=e.device;return true;}}
    return false;
}
bool RangeIsMmio(uint64_t base,uint64_t bytes){
    BootResourceClassification c{};
    constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Mmio));
    return bytes<=0xffffffffull&&AcpiClassifyBootRange(base,uint32_t(bytes),c)&&!(c.types&~allowed);
}
// BAR0 (regs), BAR1 (VRAM aperture, 64-bit) and the GOP window state that
// sizes the plan. BAR1's size is bounded by BAR3 above it (no BAR sizing writes).
bool Configure(){
    PciDevice gpu{};if(!FindGpu(gpu)){DebugChannelSend("{\"nv_disp\":\"error\",\"msg\":\"no NVIDIA display function\"}\n");return false;}
    const uint16_t cmd=PciConfigRead16(gpu.bus,gpu.dev,gpu.fn,4);
    const uint32_t b0=PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x10),b1l=PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x14),
        b1h=PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x18),b3l=PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x1c),b3h=PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x20);
    const uint64_t bar0=b0&~0xfull,bar1=(uint64_t(b1h)<<32)|(b1l&~0xfull),bar3=(uint64_t(b3h)<<32)|(b3l&~0xfull);
    if(!(cmd&2)||(b0&1)||!bar0||(b1l&1)||((b1l>>1)&3)!=2||!bar1){
        DebugChannelSend("{\"nv_disp\":\"error\",\"msg\":\"BAR0/BAR1 unassigned or memory decode off\"}\n");return false;}
    uint64_t size=256ull<<20;if(bar3>bar1&&bar3-bar1<size)size=bar3-bar1;
    if(!RangeIsMmio(bar0,16ull<<20)||!RangeIsMmio(bar1,size)){
        DebugChannelSend("{\"nv_disp\":\"error\",\"msg\":\"BAR range overlaps RAM\"}\n");return false;}
    g_backend.bar0=bar0;g_backend.bar1=bar1;g_backend.bar1Size=size;g_backend.ok=true;
    // GOP window-0 state (ASSY) sizes the surfaces; the preflight then checks it all.
    const uint32_t wsize=g_backend.rd(0x690224),params=g_backend.rd(0x69022c),planar=g_backend.rd(0x690230);
    g_runner.base=nvdisp::GopFlipConfig(wsize&0xffff,wsize>>16,(planar&0x1fff)<<6,params&0xff,size);
    g_runner.b=&g_backend;g_configured=true;
    KPrintf("nv-disp: bar0 %lx bar1 %lx (%lu MiB) gop %ux%u pitch %u fmt %x\n",
        (unsigned long)bar0,(unsigned long)bar1,(unsigned long)(size>>20),wsize&0xffff,wsize>>16,(planar&0x1fff)<<6,params&0xff);
    return true;
}
// Step 4: the VBIOS image (PROM window, read-only), head-1 mode copied from
// head 0's ARM state (same monitor model on both DP outputs), table resolve.
bool ConfigureHead(){
    if(!g_vbiosRead){
        if((g_backend.rd(0x300000)&0xffff)!=0xaa55){DebugChannelSend("{\"nv_head1\":\"error\",\"msg\":\"no VBIOS at PROM 0x300000\"}\n");return false;}
        for(uint32_t i=0;i<sizeof(g_vbios);i+=4){const uint32_t w=g_backend.rd(0x300000+i);memcpy(g_vbios+i,&w,4);}
        g_vbiosRead=true;
    }
    nvdisp::HeadConfig c{};c.base=g_runner.base;
    nvdisp::HeadMode& m=c.mode;
    m.outputResource=g_backend.rd(0x68a004);m.pclkHz=g_backend.rd(0x68a00c);m.outputScaler=g_backend.rd(0x68a014);
    m.dither=g_backend.rd(0x68a018);m.maxOutputScale=g_backend.rd(0x68a02c);m.usageBounds=g_backend.rd(0x68a030);
    m.stallLock=g_backend.rd(0x68a034);m.viewport=g_backend.rd(0x68a04c);
    m.rasterSize=g_backend.rd(0x68a064);m.syncEnd=g_backend.rd(0x68a068);m.blankEnd=g_backend.rd(0x68a06c);
    m.blankStart=g_backend.rd(0x68a070);m.vertBlank2=g_backend.rd(0x68a074);
    g_head.vb=nvdisp::Vbios{g_vbios,sizeof(g_vbios)};g_head.cfg=c;g_head.auxCh=4;
    // The hard-coded DP/IED scripts were disassembled from this exact image (90.04.0b.00.86).
    g_head.resolved=nvdisp::ResolveHeadConfig(g_head.vb,4,g_head.cfg,g_head.dpout,kVbiosHash);
    g_head.b=&g_backend;
    return true;
}

// ---- D2: the desktop presents into head 0's flipped buffers (A = GOP surface, B).
// The compositor hands the damage and its back buffer to NvPrimaryPresent (compositor
// thread, g_scanoutMappingLock held); it copies the damage plus the previous frame's
// damage (the other buffer's age) into the back buffer through a WC alias and flips.
// Any failure: the driver has already put A/"DAVE" back on screen, so returning <0
// lets the GOP copy take over.
struct NvPresenterState{
    bool active=false;uint32_t* px[2]{};uint32_t pitchPx=0,width=0,height=0;
    nvdisp::AgeRepair age{};uint32_t frames=0,busy=0,waitMaxUs=0;
};
NvPresenterState g_presenter{};
// A kernel alias of `bytes` of BAR1 at `vram`, write-combining when PAT slot 4 is WC
// (the GOP framebuffer setup), else uncached.
uint32_t* MapBar1Surface(uint64_t vram,uint64_t bytes){
    const uint64_t pages=(bytes+4095)/4096,phys=g_backend.bar1+vram;
    if(vram+bytes>g_backend.bar1Size)return nullptr;
    const uint64_t cache=VmmFramebufferWriteCombining()?VMM_LEAF_PAT:(VMM_CACHE_DISABLE|VMM_WRITE_THRU);
    const VirtualAddress va=VmmAllocPages(pages,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
    if(!va)return nullptr;
    for(uint64_t i=0;i<pages;++i){
        const VirtualAddress v(va.raw()+i*4096);
        const PhysicalAddress old=VmmVirtToPhys(KernelPageTable,v);
        VmmUnmapPage(KernelPageTable,v);if(old)PmmFreePage(old);
        if(!VmmMapPage(KernelPageTable,v,PhysicalAddress(phys+i*4096),VMM_WRITABLE|VMM_NO_EXEC|cache,MemTag::Device,KernelPid))return nullptr;
    }
    return reinterpret_cast<uint32_t*>(va.raw());
}
int NvPrimaryPresent(const uint32_t* src,uint32_t srcStride,uint32_t width,uint32_t height,const DamageRects& damage){
    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock)){++g_presenter.busy;return kPresenterBusy;}
    auto& f=g_driver.f[0];
    if(!g_presenter.active||!g_driver.Live(0)||width!=g_presenter.width||height!=g_presenter.height){
        g_presenter.active=false;g_driver.out0External=false;SpinLockRelease(lock);return -1;}
    // One frame in flight: wait (bounded) for the previous flip before reusing a buffer.
    const uint64_t t0=g_backend.nowUs();
    while(g_driver.Live(0)&&(f.state==nvdisp::FlipState::Armed||f.queued>=0)&&g_backend.nowUs()-t0<50000){g_driver.Tick();g_backend.relax();}
    const uint32_t waited=uint32_t(g_backend.nowUs()-t0);if(waited>g_presenter.waitMaxUs)g_presenter.waitMaxUs=waited;
    if(!g_driver.Live(0)){g_presenter.active=false;g_driver.out0External=false;SpinLockRelease(lock);return -1;}
    if(f.state!=nvdisp::FlipState::Idle||f.queued>=0){++g_presenter.busy;SpinLockRelease(lock);return kPresenterBusy;}
    const int8_t bb=f.Back();
    if(bb<0){SpinLockRelease(lock);return kPresenterBusy;}
    uint32_t* dst=g_presenter.px[bb];const uint32_t pitch=g_presenter.pitchPx;
    auto copy=[&](const DamageRect& r){
        const int32_t x0=r.x0<0?0:r.x0,y0=r.y0<0?0:r.y0;
        const int32_t x1=r.x1>int32_t(width)?int32_t(width):r.x1,y1=r.y1>int32_t(height)?int32_t(height):r.y1;
        if(x1<=x0||y1<=y0)return;
        for(int32_t y=y0;y<y1;++y)__builtin_memcpy(dst+uint64_t(y)*pitch+x0,src+uint64_t(y)*srcStride+x0,uint64_t(x1-x0)*4);
    };
    (void)g_presenter.age.Apply(bb,damage,width,height,copy);
    __asm__ volatile("sfence":::"memory");
    if(f.Present(bb)<0){g_driver.Tick();SpinLockRelease(lock);return g_driver.Live(0)?kPresenterBusy:-1;}
    ++g_presenter.frames;
    SpinLockRelease(lock);
    return 1;
}
const CompositorPrimaryPresenter g_presenterOps{NvPrimaryPresent};
// Panic: put A back on screen without waiting or locking (best effort).
void NvPanicHook(){
    if(!g_runnersBuilt||!g_presenter.active||!g_driver.Live(0))return;
    auto& f=g_driver.f[0];
    if(f.front==0&&f.armed<0)return;
    f.state=nvdisp::FlipState::Idle;f.queued=-1;f.armed=-1;f.paced=false;(void)f.Present(0);
}
// Called with the nv-aux lock held.
bool PresenterOnLocked(){
    if(g_presenter.active)return true;
    if(!g_driver.Up0())return false;
    const nvdisp::Surface& a=g_driver.f[0].surf[0];const nvdisp::Surface& b=g_driver.f[0].surf[1];
    const uint64_t bytes=uint64_t(a.pitch)*a.height;
    if(!g_presenter.px[0]){g_presenter.px[0]=MapBar1Surface(a.vram,bytes);g_presenter.px[1]=MapBar1Surface(b.vram,bytes);}
    if(!g_presenter.px[0]||!g_presenter.px[1]){KPrintf("nv-display: BAR1 surface mapping failed\n");(void)g_driver.Down0();return false;}
    g_presenter.pitchPx=a.pitch/4;g_presenter.width=a.width;g_presenter.height=a.height;
    g_presenter.age.Reset();   // A holds the last GOP frame; B is stale
    g_presenter.active=true;g_driver.out0External=true;
    DisplaySetPanicHook(NvPanicHook);
    KPrintf("nv-display: head 0 presents by flipping (A=GOP surface, B), WC=%d\n",VmmFramebufferWriteCombining()?1:0);
    return true;
}
void PresenterStatus(){
    nvdisp::NvDisplayDriver<LiveBackend>::Line l;
    l.raw("{\"nv_display\":\"presenter\",\"active\":");l.dec(g_presenter.active?1:0);l.raw(",\"frames\":");l.dec(g_presenter.frames);
    l.raw(",\"busy\":");l.dec(g_presenter.busy);l.raw(",\"wait_max_us\":");l.dec(g_presenter.waitMaxUs);l.raw("}\n");
    DebugChannelSend(l.t);
}
// Marker files hold "1\n" (as the Intel opt-ins).
bool MarkerSet(const char* path){auto* f=VfsOpen(path,VFS_O_STREAM);if(!f)return false;
    VnodeStat st{};uint64_t offset=0;char t[2]{};bool ok=!VfsStat(f,&st)&&!st.isDir&&!st.isSymlink&&st.size==2&&
        VfsRead(f,t,2,&offset)==2&&t[0]=='1'&&t[1]=='\n';VfsRelease(f);return ok;}
void DesktopStartThread(void*){
    SpinLock* lock=NvAuxSharedLock();
    SpinLockAcquire(lock);
    EnsureRunners();
    bool ok=Configure();
    if(ok){g_driver.b=&g_backend;g_driver.r0=&g_runner;g_driver.r1=&g_head;ok=PresenterOnLocked();}
    SpinLockRelease(lock);
    if(ok)CompositorSetPrimaryPresenter(&g_presenterOps);
    else KPrintf("nv-display: bring-up failed; the desktop stays on the GOP framebuffer\n");
}

} // namespace

bool NvDispHandle(const char* command){
    static const char prefix[]="bridge hardware nv-disp";
    const uint32_t n=sizeof(prefix)-1;
    if(strncmp(command,prefix,n)!=0||(command[n]&&command[n]!=' '))return false;
    SpinLock* lock=NvAuxSharedLock();
    SpinLockAcquire(lock);
    EnsureRunners();
    // Configure lazily (PCI config reads + a few window-state reads) unless a run is live.
    const bool headLive=g_head.st.live||g_head.st.paused;
    if(!g_configured||(!g_runner.st.live&&!g_runner.st.paused&&!headLive)){if(!Configure()){SpinLockRelease(lock);return true;}}
    const char* rest=command+n;while(*rest==' ')++rest;
    if(strncmp(rest,"driver",6)==0&&(rest[6]==0||rest[6]==' ')){
        g_driver.b=&g_backend;g_driver.r0=&g_runner;g_driver.r1=&g_head;
        const char* v=rest+6;while(*v==' ')++v;
        // `driver desktop on|off|status`: the compositor presents into head 0's flips (D2).
        if(strncmp(v,"desktop",7)==0){v+=7;while(*v==' ')++v;
            if(strncmp(v,"on",2)==0){const bool ok=PresenterOnLocked();SpinLockRelease(lock);if(ok)CompositorSetPrimaryPresenter(&g_presenterOps);PresenterStatus();return true;}
            if(strncmp(v,"off",3)==0){g_presenter.active=false;SpinLockRelease(lock);CompositorSetPrimaryPresenter(nullptr);
                SpinLockAcquire(lock);g_driver.out0External=false;(void)g_driver.Down0();SpinLockRelease(lock);PresenterStatus();return true;}
            PresenterStatus();SpinLockRelease(lock);return true;}
        if(!headLive&&!g_driver.Live(1)&&!ConfigureHead()){SpinLockRelease(lock);return true;}
        g_driver.Command(rest+6);
        SpinLockRelease(lock);return true;
    }
    if(g_driver.AnyLive()&&strncmp(rest,"status",6)!=0&&strncmp(rest,"head1 status",12)!=0){
        DebugChannelSend("{\"nv_disp\":\"refused\",\"msg\":\"the resident driver owns the display (nv-disp driver down)\"}\n");
        SpinLockRelease(lock);return true;
    }
    if(strncmp(rest,"head1",5)==0&&(rest[5]==0||rest[5]==' ')){
        if(!headLive&&!ConfigureHead()){SpinLockRelease(lock);return true;}
        g_head.Command(rest+5);
    }else if(headLive&&strncmp(rest,"status",6)!=0){
        DebugChannelSend("{\"nv_disp\":\"refused\",\"msg\":\"a head-1 run is live\"}\n");
    }else g_runner.Command(rest);
    SpinLockRelease(lock);
    return true;
}
// Desktop start (kernel.cpp, after the compositor thread): with /etc/nv-display = "1\n",
// bring head-0 flips up in a short-lived thread and hand the compositor the presenter.
void NvDisplayDesktopStart(){
    if(!MarkerSet("/etc/nv-display"))return;
    Process* t=KernelThreadCreate("nv_display",DesktopStartThread,nullptr,3);
    if(t)SchedulerAddProcess(t);else KPrintf("nv-display: thread start failed\n");
}
} // namespace brook
