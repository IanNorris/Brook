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
#include "window.h"
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

// ---- D2/D3: the desktop presents into flipped VRAM buffers on both heads.
// Output 0 = head 0 / window 0 (A = GOP surface, B, third), the compositor's primary
// presenter. Output 1 = head 1 / window 2 (C, D, third), a second compositor output at
// x = head 0's width, registered as its own output backend. Each present copies the damage
// plus what the target buffer missed since it was last written (AgeRepair) from the
// compositor's back buffer through a WC alias, then flips (mailbox, never blocks).
// Any failure: the driver has already put A/"DAVE" back on screen (head 0) or taken head 1
// down, so returning <0 lets the GOP copy take over / removes output 1.
struct NvPresenterState{
    bool active=false;uint32_t* px[3]{};uint32_t nbuf=2,pitchPx=0,width=0,height=0;
    nvdisp::AgeRepair age{};uint32_t frames=0,busy=0,noBuffer=0;bool needFull=false;
    // frame time = interval between successive presents (compositor cadence): bins
    // <18 ms (60 Hz), <35, <51, >=51; plus average and max.
    uint64_t lastUs=0,sumUs=0;uint32_t intervals=0,maxUs=0,hist[4]{};
    uint64_t cpuNs=0;uint32_t cpuMaxUs=0;   // inside the present (lock, copy, flip)
};
NvPresenterState g_presenter[nvdisp::kDriverOutputs]{};
// The compositor loop hook (completion, queued arms, wake time).
uint64_t g_pollNs=0;uint32_t g_polls=0,g_pollMaxUs=0;
// Output 1's desktop rectangle, published for lock-free Info/Count readers.
uint64_t g_head1Rect=0;   // x | w<<16 | h<<32 | pitch<<48 (pitch in 64-byte units); 0 = not presented
uint64_t PresentNowNs(){return TscClockAvailable()?TscClockReadNs():ApicTickCount()*1000000ull;}
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
void NoteFrame(NvPresenterState& p){
    const uint64_t now=g_backend.nowUs();
    if(p.lastUs){const uint32_t d=uint32_t(now-p.lastUs);
        p.sumUs+=d;++p.intervals;if(d>p.maxUs)p.maxUs=d;
        ++p.hist[d<18000?0:d<35000?1:d<51000?2:3];}
    p.lastUs=now;
}
void NoteCpu(NvPresenterState& p,uint64_t t0){
    const uint64_t d=PresentNowNs()-t0;p.cpuNs+=d;if(d/1000>p.cpuMaxUs)p.cpuMaxUs=uint32_t(d/1000);}
// Never blocks on a flip (mailbox, triple buffered): render into a free buffer (or
// reclaim the queued one), arm it if nothing is in flight, else it waits as the queued
// frame and the poll hook arms it right after the next LOADV. `src` is the output's
// top-left pixel in the compositor back buffer; `damageIn` is output-local.
int PresentOutput(uint32_t o,const uint32_t* src,uint32_t srcStride,const DamageRects& damageIn){
    const uint64_t t0=PresentNowNs();
    auto& p=g_presenter[o];
    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock)){++p.busy;return kPresenterBusy;}
    auto& f=g_driver.f[o];
    if(!p.active||!g_driver.Live(o)){SpinLockRelease(lock);return -1;}
    g_driver.Tick();
    if(!g_driver.Live(o)){SpinLockRelease(lock);return -1;}
    const int8_t bb=f.Back();
    if(bb<0){++p.noBuffer;SpinLockRelease(lock);return kPresenterBusy;}
    const uint32_t width=p.width,height=p.height;
    DamageRects full;const DamageRects* damage=&damageIn;
    if(p.needFull){full=DamageRects::Full(width,height);damage=&full;p.needFull=false;}
    uint32_t* dst=p.px[bb];const uint32_t pitch=p.pitchPx;
    auto copy=[&](const DamageRect& r){
        const int32_t x0=r.x0<0?0:r.x0,y0=r.y0<0?0:r.y0;
        const int32_t x1=r.x1>int32_t(width)?int32_t(width):r.x1,y1=r.y1>int32_t(height)?int32_t(height):r.y1;
        if(x1<=x0||y1<=y0)return;
        for(int32_t y=y0;y<y1;++y)__builtin_memcpy(dst+uint64_t(y)*pitch+x0,src+uint64_t(y)*srcStride+x0,uint64_t(x1-x0)*4);
    };
    (void)p.age.Apply(bb,*damage,width,height,copy);
    __asm__ volatile("sfence":::"memory");
    if(f.Present(bb)<0){g_driver.Tick();const bool live=g_driver.Live(o);SpinLockRelease(lock);return live?kPresenterBusy:-1;}
    ++p.frames;NoteFrame(p);
    SpinLockRelease(lock);
    NoteCpu(p,t0);
    return 1;
}
int NvPrimaryPresent(const uint32_t* src,uint32_t srcStride,uint32_t width,uint32_t height,const DamageRects& damage){
    auto& p=g_presenter[0];
    if(!p.active||width!=p.width||height!=p.height){p.active=false;g_driver.out0External=false;return -1;}
    const int r=PresentOutput(0,src,srcStride,damage);
    if(r==-1){p.active=false;g_driver.out0External=false;}
    return r;
}
// Output 1 leaves the desktop (failure or `desktop off1`): the compositor drops it from
// the layout at its next frame and the desktop shrinks back to head 0.
void Head1Unpublish(){
    if(!__atomic_exchange_n(&g_head1Rect,0,__ATOMIC_ACQ_REL))return;
    OutputNotifyChanged();CompositorWake();
}
// ---- output 1 as a compositor output backend (display_output.h)
uint32_t Head1Count(){return __atomic_load_n(&g_head1Rect,__ATOMIC_ACQUIRE)?1u:0u;}
bool Head1Info(uint32_t id,OutputInfo* out){
    const uint64_t r=__atomic_load_n(&g_head1Rect,__ATOMIC_ACQUIRE);
    if(id||!out||!r)return false;
    *out=OutputInfo{};
    out->connected=out->enabled=true;
    out->x=int32_t(r&0xffff);out->y=0;out->w=uint32_t((r>>16)&0xffff);out->h=uint32_t((r>>32)&0xffff);
    out->stride=uint32_t(r>>48)*64;out->format=OutputFormatXrgb8888;
    out->refreshMhz=g_driver.f[1].frameUs?uint32_t(1000000000ull/g_driver.f[1].frameUs):60000;
    out->scalePercent=WmGetAppliedScale();
    const char name[]="BROOK-2",make[]="nv-disp head1";
    for(uint32_t i=0;i<sizeof(name);++i)out->name[i]=name[i];
    for(uint32_t i=0;i<sizeof(make)&&i<sizeof(out->make)-1;++i)out->make[i]=make[i];
    return true;
}
int Head1Enable(uint32_t id){return id?-1:0;}
int Head1Disable(uint32_t){return -1;}
// Compositor thread, g_scanoutMappingLock held (CompositorFlipSoftware).
int Head1Present(uint32_t id,const OutputBuffer&,const DamageRects& damage){
    const uint64_t r=__atomic_load_n(&g_head1Rect,__ATOMIC_ACQUIRE);
    if(id||!r)return -1;
    if(damage.empty())return 0;
    uint32_t stride=0;const uint32_t* src=CompositorPresentSource(&stride);
    const uint32_t x=uint32_t(r&0xffff);
    if(!src||stride<x+g_presenter[1].width)return -1;
    const int res=PresentOutput(1,src+x,stride,damage);
    if(res==kPresenterBusy){g_presenter[1].needFull=true;return 0;}   // this frame's damage is redone in full
    if(res<0){g_presenter[1].active=false;g_driver.out1External=false;Head1Unpublish();return -1;}
    return res;
}
uint32_t Head1Poll(uint32_t){return uint32_t(OutputFlipState::Idle);}
const OutputBackendOps g_head1Ops={
    "nv-head1",Head1Count,Head1Info,Head1Enable,Head1Disable,nullptr,
    Head1Present,Head1Poll,nullptr,nullptr,nullptr,nullptr,
};
bool g_head1Registered=false;
// Vblank-driven compositor pacing. The loop must run once per frame, just after the LOADV
// that put the last armed flip on screen: then the flip completes, the queued frame (if
// any) is armed at once, and the frame rendered in this loop is armed straight away and
// shows at the next vblank. The previous 16 ms timeout free-ran against the 16.67 ms
// frame, so twice per ~second two loops fell in one frame (mailbox replace) and one frame
// got no new image. The wake is predicted from the head's raster position (RG scanline)
// and its measured frame period; the 1 ms tick makes the sleep land 0.3..1.3 ms past the
// LOADV, never before it. With head 1 presented and its LOADV due within 5 ms after head
// 0's, the loop waits for that one too, so neither head's flip is still armed when the
// next frame is presented to it. Returns ms; the nv-aux lock is only try-locked.
uint32_t WakeMsLocked(){
    const auto& f0=g_driver.f[0];
    uint32_t target=f0.UsToWrap(f0.lastVline);
    if(g_presenter[1].active&&g_driver.Live(1)){
        const auto& f1=g_driver.f[1];const uint32_t t1=f1.UsToWrap(f1.lastVline);
        if(t1>target&&t1-target<5000)target=t1;
        if(f1.state==nvdisp::FlipState::Armed&&f1.queued>=0&&t1<target)target=t1;
    }
    const uint32_t ms=(target+300+999)/1000+1;
    const uint32_t cap=f0.frameUs/1000+2;
    return ms>cap?cap:ms;
}
uint32_t NvPrimaryPoll(){
    const uint64_t t0=PresentNowNs();
    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock))return 2;
    uint32_t ms=16;bool lost1=false;
    if(g_presenter[0].active&&g_driver.Live(0)){
        g_driver.Tick();
        if(g_driver.Live(0))ms=WakeMsLocked();
    }
    if(g_presenter[1].active&&!g_driver.Live(1)){g_presenter[1].active=false;g_driver.out1External=false;lost1=true;}
    SpinLockRelease(lock);
    if(lost1)Head1Unpublish();
    const uint64_t d=PresentNowNs()-t0;g_pollNs+=d;++g_polls;if(d/1000>g_pollMaxUs)g_pollMaxUs=uint32_t(d/1000);
    return ms;
}
const CompositorPrimaryPresenter g_presenterOps{NvPrimaryPresent,NvPrimaryPoll};
// Panic: put A back on screen without waiting or locking (best effort).
void NvPanicHook(){
    if(!g_runnersBuilt||!g_presenter[0].active||!g_driver.Live(0))return;
    auto& f=g_driver.f[0];
    if(f.front==0&&f.armed<0)return;
    f.state=nvdisp::FlipState::Idle;f.queued=-1;f.armed=-1;f.paced=false;(void)f.Present(0);
}
bool MapPresenter(NvPresenterState& p,nvdisp::NvWindowFlipper<LiveBackend>& f,const nvdisp::Surface& third){
    const nvdisp::Surface& a=f.surf[0];
    const uint64_t bytes=uint64_t(a.pitch)*a.height;
    if(!p.px[0]){p.px[0]=MapBar1Surface(a.vram,bytes);p.px[1]=MapBar1Surface(f.surf[1].vram,bytes);}
    if(third.vram&&!p.px[2])p.px[2]=MapBar1Surface(third.vram,bytes);
    if(!p.px[0]||!p.px[1])return false;
    if(third.vram&&p.px[2])f.SetThird(third);
    p.nbuf=f.nbuf;p.pitchPx=a.pitch/4;p.width=a.width;p.height=a.height;
    p.age.Reset();p.lastUs=0;p.needFull=false;
    return true;
}
// Called with the nv-aux lock held.
bool PresenterOnLocked(){
    auto& p=g_presenter[0];
    if(p.active)return true;
    if(!g_driver.Up0())return false;
    auto& f=g_driver.f[0];
    if(!MapPresenter(p,f,g_driver.HeadZeroThird())){KPrintf("nv-display: BAR1 surface mapping failed\n");(void)g_driver.Down0();return false;}
    // A holds the last GOP frame (AgeRepair::Reset); B and the third are stale.
    p.active=true;g_driver.out0External=true;
    DisplaySetPanicHook(NvPanicHook);
    KPrintf("nv-display: head 0 presents by flipping (%u buffers), WC=%d\n",p.nbuf,VmmFramebufferWriteCombining()?1:0);
    return true;
}
// D3, nv-aux lock held: head 1 (enable + window 2, the D1 sequence) joins the desktop as
// output 1 to the right of head 0. Only next to a flipped head 0. Publishing is done by
// the caller after the lock is released (Head1Publish).
bool Presenter1OnLocked(){
    auto& p=g_presenter[1];
    if(p.active)return true;
    if(!g_presenter[0].active||!g_driver.Live(0))return false;
    if(!g_driver.Live(1)){
        if(!(g_head.st.live||g_head.st.paused)&&!ConfigureHead())return false;
        if(!g_driver.Up1())return false;
    }
    auto& f=g_driver.f[1];
    if(!MapPresenter(p,f,g_driver.HeadOneThird(f.surf[0]))){KPrintf("nv-display: head 1 BAR1 mapping failed\n");(void)g_driver.Down1();return false;}
    p.age.last[0]=-1;   // C holds the bring-up test pattern, not a desktop frame
    p.active=true;g_driver.out1External=true;
    KPrintf("nv-display: head 1 presents by flipping (%u buffers) as output 2 at x=%u\n",p.nbuf,g_presenter[0].width);
    return true;
}
void Head1Publish(){
    const auto& p0=g_presenter[0];const auto& p1=g_presenter[1];
    if(!p1.active)return;
    const uint64_t r=uint64_t(p0.width&0xffff)|(uint64_t(p1.width&0xffff)<<16)|(uint64_t(p1.height&0xffff)<<32)|(uint64_t((p1.pitchPx*4/64)&0xffff)<<48);
    if(!g_head1Registered)g_head1Registered=OutputRegisterBackend(&g_head1Ops);
    __atomic_store_n(&g_head1Rect,r,__ATOMIC_RELEASE);
    OutputNotifyChanged();CompositorWake();
}
// nv-aux lock held. The compositor stops presenting to head 1 (it sees !active under this
// lock), then head 1 goes down. The caller unpublishes after releasing the lock.
void Presenter1OffLocked(){
    g_presenter[1].active=false;g_driver.out1External=false;
    (void)g_driver.Down1();
}
void PresenterStatus(){
    for(uint32_t o=0;o<nvdisp::kDriverOutputs;++o){
        nvdisp::NvDisplayDriver<LiveBackend>::Line l;const auto& p=g_presenter[o];
        l.raw("{\"nv_display\":\"presenter\",\"output\":");l.dec(o);l.raw(",\"active\":");l.dec(p.active?1:0);l.raw(",\"buffers\":");l.dec(p.nbuf);
        l.raw(",\"frames\":");l.dec(p.frames);l.raw(",\"busy\":");l.dec(p.busy);l.raw(",\"no_buffer\":");l.dec(p.noBuffer);
        l.raw(",\"frame_avg_us\":");l.dec(p.intervals?uint32_t(p.sumUs/p.intervals):0);l.raw(",\"frame_max_us\":");l.dec(p.maxUs);
        l.raw(",\"frame_hist\":[");for(uint32_t i=0;i<4;++i){if(i)l.raw(",");l.dec(p.hist[i]);}
        l.raw("],\"cpu_ms\":");l.dec(p.cpuNs/1000000);l.raw(",\"cpu_max_us\":");l.dec(p.cpuMaxUs);l.raw("}\n");
        DebugChannelSend(l.t);
    }
}
// `driver desktop stats [reset]`: per-head counters over a window (pacing, LOADV, CPU).
struct StatsMark{uint64_t us=0,pollNs=0;uint32_t polls=0;
    uint32_t presents[2]{},completes[2]{},replaced[2]{},loadv[2]{},frames[2]{},hist[2][4]{};uint64_t vblanks[2]{},cpuNs[2]{};};
StatsMark g_mark{};
StatsMark TakeMark(){
    StatsMark m{};m.us=g_backend.nowUs();m.pollNs=g_pollNs;m.polls=g_polls;
    for(uint32_t o=0;o<2;++o){const auto& f=g_driver.f[o];const auto& p=g_presenter[o];
        m.presents[o]=f.st.presents;m.completes[o]=f.st.completes;m.replaced[o]=f.st.replaced;m.vblanks[o]=f.st.vblanks;
        m.loadv[o]=g_driver.Live(o)?g_backend.rd(nvdisp::RegLoadvCounter(o)):0;m.frames[o]=p.frames;m.cpuNs[o]=p.cpuNs;
        for(uint32_t i=0;i<4;++i)m.hist[o][i]=p.hist[i];}
    return m;
}
void PresenterStats(bool reset){
    const StatsMark now=TakeMark();
    if(reset||!g_mark.us){g_mark=now;g_pollMaxUs=0;for(auto& p:g_presenter)p.cpuMaxUs=0;}
    const StatsMark& a=g_mark;
    const uint64_t us=now.us-a.us;
    for(uint32_t o=0;o<2;++o){
        nvdisp::NvDisplayDriver<LiveBackend>::Line l;const auto& f=g_driver.f[o];
        l.raw("{\"nv_display\":\"stats\",\"output\":");l.dec(o);l.raw(",\"live\":");l.dec(g_driver.Live(o)?1:0);
        l.raw(",\"us\":");l.dec(us);l.raw(",\"frame_us\":");l.dec(f.frameUs);
        l.raw(",\"presents\":");l.dec(now.presents[o]-a.presents[o]);l.raw(",\"completes\":");l.dec(now.completes[o]-a.completes[o]);
        l.raw(",\"replaced\":");l.dec(now.replaced[o]-a.replaced[o]);l.raw(",\"vblank_bits\":");l.dec(now.vblanks[o]-a.vblanks[o]);
        l.raw(",\"loadv\":");l.dec(uint32_t(now.loadv[o]-a.loadv[o]));l.raw(",\"loadv_now\":");l.dec(now.loadv[o]);
        l.raw(",\"frames\":");l.dec(now.frames[o]-a.frames[o]);
        l.raw(",\"hist\":[");for(uint32_t i=0;i<4;++i){if(i)l.raw(",");l.dec(now.hist[o][i]-a.hist[o][i]);}
        l.raw("],\"cpu_us\":");l.dec((now.cpuNs[o]-a.cpuNs[o])/1000);l.raw(",\"cpu_max_us\":");l.dec(g_presenter[o].cpuMaxUs);l.raw("}\n");
        DebugChannelSend(l.t);
    }
    nvdisp::NvDisplayDriver<LiveBackend>::Line l;
    l.raw("{\"nv_display\":\"stats_poll\",\"us\":");l.dec(us);l.raw(",\"polls\":");l.dec(now.polls-a.polls);
    l.raw(",\"poll_cpu_us\":");l.dec((now.pollNs-a.pollNs)/1000);l.raw(",\"poll_max_us\":");l.dec(g_pollMaxUs);l.raw("}\n");
    DebugChannelSend(l.t);
}
// Marker files hold "1\n" (as the Intel opt-ins).
bool MarkerSet(const char* path){auto* f=VfsOpen(path,VFS_O_STREAM);if(!f)return false;
    VnodeStat st{};uint64_t offset=0;char t[2]{};bool ok=!VfsStat(f,&st)&&!st.isDir&&!st.isSymlink&&st.size==2&&
        VfsRead(f,t,2,&offset)==2&&t[0]=='1'&&t[1]=='\n';VfsRelease(f);return ok;}
bool FileExists(const char* path){auto* f=VfsOpen(path,VFS_O_STREAM);if(!f)return false;VfsRelease(f);return true;}
void DesktopStartThread(void*){
    SpinLock* lock=NvAuxSharedLock();
    SpinLockAcquire(lock);
    EnsureRunners();
    bool ok=Configure();
    if(ok){g_driver.b=&g_backend;g_driver.r0=&g_runner;g_driver.r1=&g_head;ok=PresenterOnLocked();}
    SpinLockRelease(lock);
    if(ok)CompositorSetPrimaryPresenter(&g_presenterOps);
    else{KPrintf("nv-display: bring-up failed; the desktop stays on the GOP framebuffer\n");return;}
    // D3: the second monitor (conn2) joins unless /etc/nv-no-second-head exists. The head-1
    // preflight reads the sink's DPCD first and refuses without one; DP training takes
    // ~1-2 s, during which head 0 presents are deferred (try-lock busy).
    if(FileExists("/etc/nv-no-second-head"))return;
    SpinLockAcquire(lock);
    const bool on1=Presenter1OnLocked();
    SpinLockRelease(lock);
    if(on1)Head1Publish();
    else KPrintf("nv-display: head 1 bring-up failed or no sink; the desktop stays on head 0\n");
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
        // `driver desktop on|off|on1|off1|status|stats [reset]`: the compositor presents into
        // head 0's flips (D2) and head 1's as a second output (D3). `off` takes both down.
        if(strncmp(v,"desktop",7)==0){v+=7;while(*v==' ')++v;
            if(strncmp(v,"on1",3)==0){const bool ok=Presenter1OnLocked();SpinLockRelease(lock);if(ok)Head1Publish();PresenterStatus();return true;}
            if(strncmp(v,"off1",4)==0){Presenter1OffLocked();SpinLockRelease(lock);Head1Unpublish();PresenterStatus();return true;}
            if(strncmp(v,"on",2)==0){const bool ok=PresenterOnLocked();SpinLockRelease(lock);if(ok)CompositorSetPrimaryPresenter(&g_presenterOps);PresenterStatus();return true;}
            if(strncmp(v,"off",3)==0){
                if(g_presenter[1].active)Presenter1OffLocked();
                g_presenter[0].active=false;SpinLockRelease(lock);Head1Unpublish();CompositorSetPrimaryPresenter(nullptr);
                SpinLockAcquire(lock);g_driver.out0External=false;(void)g_driver.Down0();SpinLockRelease(lock);PresenterStatus();return true;}
            if(strncmp(v,"stats",5)==0){v+=5;while(*v==' ')++v;PresenterStats(strncmp(v,"reset",5)==0);SpinLockRelease(lock);return true;}
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
