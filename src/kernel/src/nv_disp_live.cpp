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
#include "nv_ce_present_api.h"
#include "nv_disp_hole.h"
#include "nv_drm.h"
#include "nv_layer.h"
#if defined(BROOK_NV_GSP)
#include "nv_gsp_live.h"
#endif
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
    // 27 Nov NVK head freeze: a BAR1 flush (0x070000) is preceded by a BAR1 read of the last
    // BAR1-written word, so every plan/flipper kick that flushes is ordered behind its ring writes
    // (with GSP-RM up BAR1 is virtual and a posted write can wait on a page walk).
    uint64_t lastVwr=~0ull;uint32_t flushReads=0;
    void wr(uint32_t a,uint32_t v){if(a==0x070000u&&lastVwr!=~0ull){(void)vrd(lastVwr);++flushReads;}
        volatile uint32_t* p=regs.Map(bar0+a);if(p)*p=v;}
    uint32_t vrd(uint64_t a){if(a+4>bar1Size)return 0xffffffffu;volatile uint32_t* p=VramMap(a);return p?*p:0xffffffffu;}
    void vwr(uint64_t a,uint32_t v){if(a+4>bar1Size)return;volatile uint32_t* p=VramMap(a);if(p){*p=v;lastVwr=a;}}
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
    void emit(const char* line){
        if(toSerial)SerialPuts(line);else DebugChannelSend(line);
        // Driver failures also go to the kernel log (hw_logs): the debug channel copy is lost
        // when nobody is attached (26 Nov head-0 freeze).
        auto pre=[line](const char* p){return strncmp(line,p,strlen(p))==0;};
        if(!toSerial&&(pre("{\"nv_driver\":\"flip-failed")||pre("{\"nv_driver\":\"up")||pre("{\"nv_disp\":\"aborted\"")||
                       pre("{\"nv_driver\":\"fence")))KPuts(line);
    }
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
void SnapOnFail(uint32_t o);
void EnsureRunners(){
    if(g_runnersBuilt)return;
    new(g_runnerStorage) RunnerT{};
    new(g_headStorage) HeadT{};
    new(g_driverStorage) DriverT{};
    reinterpret_cast<DriverT*>(g_driverStorage)->onFailDiag=SnapOnFail;
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
    // RAM copy of what this head shows (ShadowDiff); invalid until the first full present.
    uint32_t* shadow=nullptr;bool shadowValid=false;
    DamageRects pending{};uint32_t deferred=0;   // changed (shadow updated) but not yet in VRAM
    uint64_t compared=0,copiedBytes=0,diffNs=0,copyNs=0;uint32_t unchanged=0,fullDiffs=0,copyMaxUs=0;
    uint64_t vram[3]{};   // VRAM (== BAR1) offsets of px[]
    // CE present path (head 0, `nv-gsp ce-present on`): frames the copy engine wrote, frames
    // it gave back to the CPU, and the read-back check of sampled pixels.
    uint64_t ceFrames=0,ceBytes=0,ceCpuFrames=0;uint32_t ceVerified=0,ceMismatch=0;
    uint64_t glassFrames=0,glassBytes=0;
    // A kicked copy (shadow -> px[ceBb]) not yet flipped; its rectangles for a CPU fallback.
    bool ceInflight=false,shadowContig=false;int8_t ceBb=-1;uint32_t ceTicket=0,ceN=0;NvCeRect ceRects[100]{};
    uint64_t ceWaitNs=0,ceKicks=0,ceFlipsFromPoll=0;
    // Zero-copy step 2 (head 0): the hole (output-local; empty = none) and whose layer fills it,
    // the layer frame (seq) the last flip copied, and, for the kicked copy, the layer it reads.
    DamageRect hole{};uint32_t holeSlot=0;uint64_t holeIdentity=0,holeSeqFlipped=0;bool holeFlipped=false;
    bool ceLayer=false;DamageRect ceLayerRect{};uint64_t ceLayerPhys=0;uint32_t ceLayerStride=0;
    uint64_t directFrames=0,layerCeFrames=0,layerCpuFrames=0,holeRepairs=0,layerMissing=0,layerVerified=0;
};
NvPresenterState g_presenter[nvdisp::kDriverOutputs]{};
const NvCeOps* g_ceOps=nullptr;       // set/cleared under the nv-aux lock
// If the CE semaphore times out, the engine may still access both BOs.
void* g_glassUncertainSource=nullptr; // keep a single quarantined PRIME until reboot
int8_t g_glassUncertainDestination=-1;
bool g_ceVerifyOff=false;             // a read-back mismatch turned the CE path off this boot
// The compositor loop hook (completion, queued arms, wake time).
uint64_t g_pollNs=0;uint32_t g_polls=0,g_pollMaxUs=0,g_pollBusy=0;
// Loop timing (pacing diagnosis): the first head-0 present after each poll, against the
// wake target the poll set: early (a client commit woke the loop) or late by <1/<2/<4/<8/
// >=8 ms; the scanline it arms at (8 bins of vtotal); and the loop's busy time, first
// present to the next poll: <2/<4/<8/<16/>=16 ms.
uint32_t g_wakeHist[6]{},g_armLineHist[8]{},g_busyHist[5]{};uint64_t g_loopFirstUs=0;bool g_loopPresented=false;
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
// ---- CE present (head 0): finish the kicked copy. Waits up to budgetUs (0 = check),
// spot-checks it through BAR1 against the shadow, CPU-copies its rectangles if the copy
// failed, then flips. False = still running (only with a short budget).
constexpr uint32_t kCeWaitUs=100000;
bool CeFinishLocked(uint32_t budgetUs){
    auto& p=g_presenter[0];
    if(!p.ceInflight)return true;
    auto& f=g_driver.f[0];
    const uint64_t w0=PresentNowNs();
    const int r=g_ceOps?g_ceOps->wait(p.ceTicket,budgetUs):-1;
    p.ceWaitNs+=PresentNowNs()-w0;
    if(r==0)return false;
    uint32_t* dst=p.px[p.ceBb];const uint32_t pitch=p.pitchPx,width=p.width,height=p.height;
    auto clip=[&](const NvCeRect& q,int32_t* x0,int32_t* y0,int32_t* x1,int32_t* y1){
        *x0=q.x0<0?0:q.x0;*y0=q.y0<0?0:q.y0;*x1=q.x1>int32_t(width)?int32_t(width):q.x1;*y1=q.y1>int32_t(height)?int32_t(height):q.y1;
        return *x1>*x0&&*y1>*y0;};
    auto cpu=[&](const NvCeRect& q){int32_t x0,y0,x1,y1;if(!clip(q,&x0,&y0,&x1,&y1))return;
        for(int32_t y=y0;y<y1;++y)__builtin_memcpy(dst+uint64_t(y)*pitch+x0,p.shadow+uint64_t(y)*width+x0,uint64_t(x1-x0)*4);};
    // The layer is still held: its retire (nv_layer.cpp) waits for this function under the lock.
    const uint32_t* bo=p.ceLayer?reinterpret_cast<const uint32_t*>(PhysToVirt(PhysicalAddress(p.ceLayerPhys)).raw()):nullptr;
    auto cpuLayer=[&]{if(bo){nvdisp::CopyLayerRect(dst,pitch,p.ceLayerRect,bo,p.ceLayerStride/4);++p.layerCpuFrames;}};
    if(r==1){
        ++p.ceFrames;
        if(p.ceFrames<=300||!(p.ceFrames&63)){
            for(uint32_t i=0;i<p.ceN&&i<4;++i){int32_t x0,y0,x1,y1;if(!clip(p.ceRects[i],&x0,&y0,&x1,&y1))continue;
                const int32_t xs[2]={x0,x1-1},ys[2]={y0,y1-1};
                for(int a=0;a<2;++a)for(int b2=0;b2<2;++b2){++p.ceVerified;
                    if(dst[uint64_t(ys[b2])*pitch+xs[a]]!=p.shadow[uint64_t(ys[b2])*width+xs[a]]){++p.ceMismatch;g_ceVerifyOff=true;}}}
            if(bo){const DamageRect& h=p.ceLayerRect;const uint32_t sp=p.ceLayerStride/4;
                const int32_t xs[2]={h.x0,h.x1-1},ys[2]={h.y0,h.y1-1};
                for(int a=0;a<2;++a)for(int b2=0;b2<2;++b2){++p.layerVerified;
                    if(dst[uint64_t(ys[b2])*pitch+xs[a]]!=(bo[uint64_t(ys[b2]-h.y0)*sp+uint32_t(xs[a]-h.x0)]|0xff000000u)){++p.ceMismatch;g_ceVerifyOff=true;}}}
            if(g_ceVerifyOff){KPrintf("nv-display: CE present read-back mismatch; CPU copies from now on\n");
                for(uint32_t i=0;i<p.ceN;++i)cpu(p.ceRects[i]);
                cpuLayer();}
            else if(bo)++p.layerCeFrames;
        }else if(bo)++p.layerCeFrames;
    }else{g_ceOps=nullptr;++p.ceCpuFrames;KPrintf("nv-display: CE present copy failed; CPU copies from now on\n");
        for(uint32_t i=0;i<p.ceN;++i)cpu(p.ceRects[i]);
        cpuLayer();}
    p.ceInflight=false;p.ceLayer=false;
    __asm__ volatile("sfence":::"memory");
    if(f.Present(p.ceBb)<0){g_driver.Tick();return true;}
    ++p.frames;NoteFrame(p);
    return true;
}
// Never blocks on a flip (mailbox, triple buffered): render into a free buffer (or
// reclaim the queued one), arm it if nothing is in flight, else it waits as the queued
// frame and the poll hook arms it right after the next LOADV. `src` is the output's
// top-left pixel in the compositor back buffer; `damageIn` is output-local.
// nv-aux lock held, flip idle: copy what the back buffer `bb` lacks (its missed damage plus
// the pending changes) and flip it. 1 = flipped, 0 = no free buffer (pending kept), -1 = dead.
// Head-0 flip fence (26 Nov head-0 freeze: window 0 re-armed with a foreign ISO ctxdma during
// the GSP bring-up). While set, head-0 damage is kept pending and no flip is submitted.
volatile bool g_head0Fence=false;
int FlipPendingLocked(uint32_t o,const uint32_t* src,uint32_t srcStride){
    auto& p=g_presenter[o];auto& f=g_driver.f[o];
    const int8_t bb=f.Back(o==0?g_glassUncertainDestination:-1);
    if(bb<0){++p.noBuffer;return 0;}
    const uint32_t width=p.width,height=p.height;
    uint32_t* dst=p.px[bb];const uint32_t pitch=p.pitchPx;
    uint64_t bytes=0;const uint64_t c0=PresentNowNs();
    auto copy=[&](const DamageRect& r){
        const int32_t x0=r.x0<0?0:r.x0,y0=r.y0<0?0:r.y0;
        const int32_t x1=r.x1>int32_t(width)?int32_t(width):r.x1,y1=r.y1>int32_t(height)?int32_t(height):r.y1;
        if(x1<=x0||y1<=y0)return;
        for(int32_t y=y0;y<y1;++y)__builtin_memcpy(dst+uint64_t(y)*pitch+x0,src+uint64_t(y)*srcStride+x0,uint64_t(x1-x0)*4);
        bytes+=uint64_t(x1-x0)*4*uint32_t(y1-y0);
    };
    const bool ce=o==0&&g_ceOps&&!g_ceVerifyOff&&p.shadow&&p.shadowValid;
    if(ce&&p.ceInflight){(void)CeFinishLocked(kCeWaitUs);return 1;}   // that frame flips now; pending waits for the next idle poll
    bool kicked=false;
    // Zero-copy step 2: the hole is left out of every copy and filled from the window's layer
    // last (looked up under the nv-aux lock, which its retire takes, so it stays valid until
    // the copy is finished). A vanished layer leaves the hole as the copies left it.
    NvLayer layer{};DamageRect lrect{};
    const DamageRect hole=o==0?p.hole:DamageRect{};
    if(!hole.empty()){
        if(NvLayerGet(p.holeSlot,p.holeIdentity,&layer,false)){
            lrect=DamageRect{hole.x0,hole.y0,hole.x0+int32_t(layer.w),hole.y0+int32_t(layer.h)};
            if(lrect.x1>hole.x1)lrect.x1=hole.x1;
            if(lrect.y1>hole.y1)lrect.y1=hole.y1;
            if(lrect.empty())layer=NvLayer{};
        }else ++p.layerMissing;
    }
    const uint32_t* bo=layer.phys?reinterpret_cast<const uint32_t*>(PhysToVirt(PhysicalAddress(layer.phys)).raw()):nullptr;
    auto copyOutside=[&](const DamageRect& r){nvdisp::ForEachOutside(r,hole,copy);};
    if(ce){
        // Collect the rectangles and kick one copy from the shadow (== this frame); the flip
        // happens when it lands, while the compositor composes the next frame. More than
        // 100 rectangles (99 with a layer), or a refused kick: the CPU copies them from the
        // back buffer.
        uint32_t n=0;bool over=false;const uint32_t cap=bo?99:100;
        auto collect1=[&](const DamageRect& r){if(n<cap)p.ceRects[n++]=NvCeRect{r.x0,r.y0,r.x1,r.y1};else{over=true;copy(r);}};
        auto collect=[&](const DamageRect& r){nvdisp::ForEachOutside(r,hole,collect1);};
        (void)p.age.Apply(bb,p.pending,width,height,collect);
        uint64_t ceBytes=0;uint32_t ticket=0;int r=0;
        if((n||bo)&&!over){NvCeJob job{p.shadow,width,width,height,p.vram[bb],pitch*4,p.ceRects,n};
            if(bo){job.layer_phys=layer.phys;job.layer_stride=layer.stride;job.layer_dst=NvCeRect{lrect.x0,lrect.y0,lrect.x1,lrect.y1};}
            r=g_ceOps->kick(job,&ceBytes,&ticket);}
        if(r==1){kicked=true;bytes+=ceBytes;p.ceBytes+=ceBytes;++p.ceKicks;p.ceInflight=true;p.ceBb=bb;p.ceTicket=ticket;p.ceN=n;
            p.ceLayer=bo!=nullptr;p.ceLayerRect=lrect;p.ceLayerPhys=layer.phys;p.ceLayerStride=layer.stride;}
        else{if(r<0){g_ceOps=nullptr;KPrintf("nv-display: CE present kick failed; CPU copies from now on\n");}
            if(n)++p.ceCpuFrames;for(uint32_t i=0;i<n;++i)copy(DamageRect{p.ceRects[i].x0,p.ceRects[i].y0,p.ceRects[i].x1,p.ceRects[i].y1});
            if(bo){nvdisp::CopyLayerRect(dst,pitch,lrect,bo,layer.stride/4);++p.layerCpuFrames;bytes+=uint64_t(lrect.x1-lrect.x0)*4*uint32_t(lrect.y1-lrect.y0);}}
    }else{
        (void)p.age.Apply(bb,p.pending,width,height,copyOutside);
        if(bo){nvdisp::CopyLayerRect(dst,pitch,lrect,bo,layer.stride/4);++p.layerCpuFrames;bytes+=uint64_t(lrect.x1-lrect.x0)*4*uint32_t(lrect.y1-lrect.y0);}
    }
    if(!hole.empty()){p.holeSeqFlipped=layer.seq;p.holeFlipped=true;++p.directFrames;}
    p.pending=DamageRects{};
    __asm__ volatile("sfence":::"memory");
    {const uint64_t c1=PresentNowNs();p.copyNs+=c1-c0;p.copiedBytes+=bytes;if((c1-c0)/1000>p.copyMaxUs)p.copyMaxUs=uint32_t((c1-c0)/1000);}
    if(kicked)return 1;   // CeFinishLocked flips it
    if(f.Present(bb)<0){g_driver.Tick();return g_driver.Live(o)?0:-1;}
    ++p.frames;NoteFrame(p);
    return 1;
}
// Never blocks on a flip. The damage is first diffed against the head's shadow; what
// really changed joins the head's pending changes. With nothing in flight they are copied
// and flipped now (shown at the next vblank); with a flip still armed (a client commit
// woke the loop mid-frame) they wait, and the poll after the LOADV flips them, so each
// vblank costs at most one copy, always into the buffer that missed the fewest frames
// (D2's mailbox rendered such frames into the third buffer, which then needed the whole
// drag history: 10-16 ms copies). `src` is the output's top-left pixel in the
// compositor back buffer; `damageIn` is output-local.
// Zero-copy step 2: the hole the compositor asked for this frame (compositor thread; applied
// by the next head-0 present that gets the lock).
CompositorDirectHole g_holeReq{};bool g_holeReqOn=false;
volatile bool g_directOff=false;   // `nv-disp direct off`: windows are composed as before
// The layer filling the hole has a frame the last flip did not copy, or the hole changed.
bool HoleNeedsFlip(const NvPresenterState& p){
    if(p.hole.empty())return false;
    if(!p.holeFlipped)return true;
    NvLayer l{};
    return NvLayerGet(p.holeSlot,p.holeIdentity,&l,false)&&l.seq!=p.holeSeqFlipped;
}
int PresentOutput(uint32_t o,const uint32_t* src,uint32_t srcStride,const DamageRects& damageIn){
    const uint64_t t0=PresentNowNs();
    auto& p=g_presenter[o];
    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock)){++p.busy;return kPresenterBusy;}
    auto& f=g_driver.f[o];
    if(!p.active||!g_driver.Live(o)){SpinLockRelease(lock);return -1;}
    g_driver.Tick();
    if(!g_driver.Live(o)){SpinLockRelease(lock);return -1;}
    const uint32_t width=p.width,height=p.height;
    if(o==0&&p.ceInflight)(void)CeFinishLocked(kCeWaitUs);   // the copy reads the shadow the diff is about to update
    DamageRects input=damageIn;
    if(p.needFull){input=DamageRects::Full(width,height);p.needFull=false;}
    // Only what really changed against the shadow goes over the link (D3).
    DamageRects changed;
    const uint64_t d0=PresentNowNs();
    if(o==0){
        // Zero-copy step 2: take the requested hole (clipped to the head). The old one, where
        // it is no longer a hole, is refreshed into the shadow and rewritten in every buffer.
        DamageRect want{};
        if(g_holeReqOn)want=nvdisp::ClipRect(DamageRect{g_holeReq.x0,g_holeReq.y0,g_holeReq.x1,g_holeReq.y1},width,height);
        const DamageRect was=p.hole;
        const bool other=!(want==was)||(!want.empty()&&(g_holeReq.slot!=p.holeSlot||g_holeReq.identity!=p.holeIdentity));
        if(other){
            if(p.shadow&&p.shadowValid&&nvdisp::RepairHole(src,srcStride,p.shadow,width,width,height,was,DamageRect{},changed))++p.holeRepairs;
            p.hole=want;p.holeSlot=g_holeReqOn?g_holeReq.slot:0;p.holeIdentity=g_holeReqOn?g_holeReq.identity:0;p.holeFlipped=false;
        }
    }
    if(!p.shadow||!p.shadowValid){
        changed=DamageRects::Full(width,height);
        if(p.shadow){for(uint32_t y=0;y<height;++y)__builtin_memcpy(p.shadow+uint64_t(y)*width,src+uint64_t(y)*srcStride,uint64_t(width)*4);p.shadowValid=true;}
    }else{
        if(input.full){input=DamageRects::Rows(0,height,width);++p.fullDiffs;}
        const DamageRect hole=o==0?p.hole:DamageRect{};
        for(uint32_t i=0;i<input.count;++i)p.compared+=nvdisp::HoleDiff(src,srcStride,p.shadow,width,width,height,input.rects[i],hole,changed);
    }
    p.diffNs+=PresentNowNs()-d0;
    p.pending.AddAll(changed);
    if(p.pending.empty()&&!p.pending.full&&!(o==0&&HoleNeedsFlip(p))){++p.unchanged;SpinLockRelease(lock);NoteCpu(p,t0);return 0;}
    if(changed.empty()&&!changed.full)++p.unchanged;
    if(f.state==nvdisp::FlipState::Armed||(o==0&&__atomic_load_n(&g_head0Fence,__ATOMIC_ACQUIRE))){++p.deferred;SpinLockRelease(lock);NoteCpu(p,t0);return 1;}
    const int r=FlipPendingLocked(o,src,srcStride);
    SpinLockRelease(lock);
    NoteCpu(p,t0);
    return r;
}
uint64_t g_wakeAtUs=0;uint32_t g_wakePeriodUs=16667;
void NoteLoopPresent(){
    if(g_loopPresented)return;
    g_loopPresented=true;const uint64_t now=g_backend.nowUs();g_loopFirstUs=now;
    if(g_wakeAtUs){const int64_t late=int64_t(now)-int64_t(g_wakeAtUs);
        ++g_wakeHist[late<0?0:late<1000?1:late<2000?2:late<4000?3:late<8000?4:5];}
    const auto& f=g_driver.f[0];
    if(f.vtotal)++g_armLineHist[uint64_t(g_backend.rd(nvdisp::RegRgPos(0))&0xffff)*8/f.vtotal>7?7:uint64_t(g_backend.rd(nvdisp::RegRgPos(0))&0xffff)*8/f.vtotal];
}
int NvPrimaryPresent(const uint32_t* src,uint32_t srcStride,uint32_t width,uint32_t height,const DamageRects& damage){
    auto& p=g_presenter[0];
    NoteLoopPresent();
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
// The last wake target (absolute us) and head 0's frame period, for a poll that finds the
// lock busy (dp-idle's AUX link watch): it sleeps to the same LOADV instead of polling
// again in 2 ms, which would run an extra compositor frame inside the current one.
uint32_t MsUntilUs(uint64_t atUs,uint64_t nowUs){
    const uint32_t ms=atUs>nowUs?uint32_t((atUs-nowUs+999)/1000)+1:1;
    return ms>g_wakePeriodUs/1000+2?g_wakePeriodUs/1000+2:ms;}
uint32_t WakeMsLocked(){
    const auto& f0=g_driver.f[0];
    uint32_t target=f0.UsToWrap(f0.lastVline);
    if(g_presenter[1].active&&g_driver.Live(1)){
        const auto& f1=g_driver.f[1];const uint32_t t1=f1.UsToWrap(f1.lastVline);
        if(t1>target&&t1-target<5000)target=t1;
        if(f1.state==nvdisp::FlipState::Armed&&f1.queued>=0&&t1<target)target=t1;
    }
    const uint64_t now=g_backend.nowUs();
    g_wakePeriodUs=f0.frameUs?f0.frameUs:16667;g_wakeAtUs=now+target+300;
    return MsUntilUs(g_wakeAtUs,now);
}
uint32_t WakeMsUnlocked(){
    const uint64_t now=g_backend.nowUs();
    if(!g_wakeAtUs)return 2;
    uint64_t at=g_wakeAtUs;
    if(at<=now)at+=((now-at)/g_wakePeriodUs+1)*g_wakePeriodUs;   // the same phase, next frame
    return MsUntilUs(at,now);
}
uint32_t NvPrimaryPoll(){
    const uint64_t t0=PresentNowNs();
    if(g_loopPresented){const uint64_t busy=g_backend.nowUs()-g_loopFirstUs;
        ++g_busyHist[busy<2000?0:busy<4000?1:busy<8000?2:busy<16000?3:4];g_loopPresented=false;}
    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock)){++g_pollBusy;return g_presenter[0].active?WakeMsUnlocked():2;}
    uint32_t ms=16;bool lost1=false;
    if(g_presenter[0].active&&g_driver.Live(0)){
        g_driver.Tick();
        if(g_presenter[0].ceInflight&&CeFinishLocked(0))++g_presenter[0].ceFlipsFromPoll;
        // Changes held back while a flip was armed go out once it has landed. The back
        // buffer equals the shadow here (between frames), so it is the copy source.
        for(uint32_t o=0;o<2;++o){
            auto& p=g_presenter[o];
            if(!p.active||!g_driver.Live(o)||(p.pending.empty()&&!p.pending.full&&!(o==0&&HoleNeedsFlip(p)))||g_driver.f[o].state!=nvdisp::FlipState::Idle||p.ceInflight)continue;
            if(o==0&&__atomic_load_n(&g_head0Fence,__ATOMIC_ACQUIRE))continue;
            uint32_t stride=0;const uint32_t* src=CompositorPresentSource(&stride);
            const uint32_t x=o?uint32_t(__atomic_load_n(&g_head1Rect,__ATOMIC_ACQUIRE)&0xffff):0;
            if(!src||(o&&!x)||stride<x+p.width)continue;
            (void)FlipPendingLocked(o,src+x,stride);
        }
        if(g_driver.Live(0))ms=WakeMsLocked();
        if(g_presenter[0].ceInflight)ms=1;   // look again soon: the copy lands in a few ms
    }
    if(g_presenter[1].active&&!g_driver.Live(1)){g_presenter[1].active=false;g_driver.out1External=false;lost1=true;}
    SpinLockRelease(lock);
    if(lost1)Head1Unpublish();
    const uint64_t d=PresentNowNs()-t0;g_pollNs+=d;++g_polls;if(d/1000>g_pollMaxUs)g_pollMaxUs=uint32_t(d/1000);
    return ms;
}
// Zero-copy step 2 hooks (compositor thread).
bool NvDirectAvailable(){const auto& p=g_presenter[0];
    return !g_directOff&&p.active&&p.shadow&&g_ceOps&&!g_ceVerifyOff&&g_driver.Live(0);}
void NvSetHole(const CompositorDirectHole* h){if(h){g_holeReq=*h;g_holeReqOn=true;}else g_holeReqOn=false;}
// A captured frame: the hole as the front buffer shows it (through BAR1), alpha forced.
void NvCaptureHole(uint32_t* dst,uint32_t dstStride,uint32_t width,uint32_t height){
    SpinLock* lock=NvAuxSharedLock();
    SpinLockAcquire(lock);
    const auto& p=g_presenter[0];const auto& f=g_driver.f[0];
    if(p.active&&g_driver.Live(0)&&!p.hole.empty()&&f.front>=0&&f.front<3&&p.px[f.front]){
        const DamageRect c=nvdisp::ClipRect(p.hole,width<p.width?width:p.width,height<p.height?height:p.height);
        const uint32_t* s2=p.px[f.front];
        for(int32_t y=c.y0;y<c.y1;++y)for(int32_t x=c.x0;x<c.x1;++x)
            dst[uint64_t(y)*dstStride+x]=s2[uint64_t(y)*p.pitchPx+x]|0xff000000u;
    }
    SpinLockRelease(lock);
}
// nv-aux lock held (nv_layer.cpp, after a layer swap): no kicked copy reads the old layer after this.
void NvLayerRetireLocked(){if(g_presenter[0].ceInflight)(void)CeFinishLocked(kCeWaitUs);}
CompositorPrimaryPresenter MakePresenterOps(){CompositorPrimaryPresenter o{NvPrimaryPresent,NvPrimaryPoll};
    o.directAvailable=NvDirectAvailable;o.setHole=NvSetHole;o.captureHole=NvCaptureHole;return o;}
const CompositorPrimaryPresenter g_presenterOps=MakePresenterOps();
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
    p.vram[0]=a.vram;p.vram[1]=f.surf[1].vram;p.vram[2]=third.vram;
    p.nbuf=f.nbuf;p.pitchPx=a.pitch/4;p.width=a.width;p.height=a.height;
    p.age.Reset();p.lastUs=0;p.needFull=false;
    if(!p.shadow){
        // Physically contiguous and 2 MiB aligned when the PMM has the run (the CE present
        // path maps it with 2 MiB PTEs); scattered pages otherwise.
        const uint64_t sb=uint64_t(a.width)*a.height*4,big=(sb+0x1fffff)&~0x1fffffull;
        const PhysicalAddress ph=PmmAllocPages(big/4096+512,MemTag::Device,KernelPid);
        if(ph){const uint64_t al=(ph.raw()+0x1fffff)&~0x1fffffull;p.shadow=reinterpret_cast<uint32_t*>(PhysToVirt(PhysicalAddress(al)).raw());p.shadowContig=true;}
        else{const VirtualAddress va=VmmAllocPages((sb+4095)/4096,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
            p.shadow=va?reinterpret_cast<uint32_t*>(va.raw()):nullptr;}}
    p.shadowValid=false;   // the first present copies (and records) the whole head
    p.pending=DamageRects{};
    return true;
}
// Called with the nv-aux lock held.
bool PresenterOnLocked(){
    auto& p=g_presenter[0];
    if(p.active)return true;
    // An abandoned Glass destination still belongs to the old CE work.
    if(g_glassUncertainSource)return false;
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
    // C holds the bring-up test pattern and the others uninitialised VRAM; until the desktop's
    // first head-1 present (after /nix mounts: seconds, or a long boot-time e2fsck) head 1 would
    // show them (27 Nov: "full-screen corruption on the second monitor during the disk repair").
    // Clear every head-1 buffer to black first. The age repair still treats them as stale.
    {const uint64_t px=uint64_t(p.pitchPx)*p.height;
     for(uint32_t b=0;b<3;++b)if(p.px[b])for(uint64_t i=0;i<px;++i)p.px[b][i]=0xff000000u;
     __asm__ volatile("sfence":::"memory");}
    p.age.Stale(0);   // C held the bring-up test pattern, not a desktop frame
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
    const bool zero=reset||!g_mark.us;
    if(zero){g_mark=now;g_pollMaxUs=0;for(auto& p:g_presenter){p.cpuMaxUs=0;p.compared=p.copiedBytes=p.diffNs=p.copyNs=0;p.unchanged=p.fullDiffs=p.copyMaxUs=p.deferred=0;
            p.ceFrames=p.ceBytes=p.ceCpuFrames=p.glassFrames=p.glassBytes=0;p.ceVerified=p.ceMismatch=0;p.ceWaitNs=p.ceKicks=p.ceFlipsFromPoll=0;}
        for(auto& v:g_wakeHist)v=0;for(auto& v:g_armLineHist)v=0;for(auto& v:g_busyHist)v=0;}
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
    for(uint32_t o=0;o<2;++o){
        nvdisp::NvDisplayDriver<LiveBackend>::Line c;const auto& p=g_presenter[o];
        c.raw("{\"nv_display\":\"stats_copy\",\"output\":");c.dec(o);c.raw(",\"compared_px\":");c.dec(p.compared);
        c.raw(",\"copied_bytes\":");c.dec(p.copiedBytes);c.raw(",\"diff_us\":");c.dec(p.diffNs/1000);c.raw(",\"copy_us\":");c.dec(p.copyNs/1000);
        c.raw(",\"copy_max_us\":");c.dec(p.copyMaxUs);c.raw(",\"unchanged\":");c.dec(p.unchanged);c.raw(",\"full_diffs\":");c.dec(p.fullDiffs);c.raw(",\"deferred\":");c.dec(p.deferred);
        c.raw(",\"busy\":");c.dec(p.busy);c.raw(",\"no_buffer\":");c.dec(p.noBuffer);
        c.raw("}\n");DebugChannelSend(c.t);
        if(o==0){   // CE present counters on their own line (the copy line's buffer is full)
            nvdisp::NvDisplayDriver<LiveBackend>::Line ce;
            ce.raw("{\"nv_display\":\"stats_ce\",\"output\":0");
            ce.raw(",\"ce\":");ce.dec(g_ceOps&&o==0&&!g_ceVerifyOff?1:0);ce.raw(",\"ce_kicks\":");ce.dec(p.ceKicks);
            ce.raw(",\"ce_wait_us\":");ce.dec(p.ceWaitNs/1000);ce.raw(",\"ce_poll_flips\":");ce.dec(p.ceFlipsFromPoll);ce.raw(",\"shadow_contig\":");ce.dec(p.shadowContig?1:0);ce.raw(",\"ce_frames\":");ce.dec(p.ceFrames);ce.raw(",\"ce_bytes\":");ce.dec(p.ceBytes);
            ce.raw(",\"ce_cpu_frames\":");ce.dec(p.ceCpuFrames);ce.raw(",\"ce_verified\":");ce.dec(p.ceVerified);ce.raw(",\"ce_mismatch\":");ce.dec(p.ceMismatch);
            ce.raw(",\"glass_frames\":");ce.dec(p.glassFrames);ce.raw(",\"glass_bytes\":");ce.dec(p.glassBytes);
            ce.raw(",\"glass_quarantined\":");ce.dec(g_glassUncertainSource?1:0);
            ce.raw("}\n");DebugChannelSend(ce.t);
            nvdisp::NvDisplayDriver<LiveBackend>::Line zc;const NvLayerStats ls=NvLayerStatsGet();
            zc.raw("{\"nv_display\":\"stats_direct\",\"direct_frames\":");zc.dec(p.directFrames);zc.raw(",\"layer_ce\":");zc.dec(p.layerCeFrames);
            zc.raw(",\"layer_cpu\":");zc.dec(p.layerCpuFrames);zc.raw(",\"hole_repairs\":");zc.dec(p.holeRepairs);zc.raw(",\"layer_missing\":");zc.dec(p.layerMissing);
            zc.raw(",\"layer_verified\":");zc.dec(p.layerVerified);zc.raw(",\"hole\":[");zc.dec(uint32_t(p.hole.x0));zc.raw(",");zc.dec(uint32_t(p.hole.y0));zc.raw(",");zc.dec(uint32_t(p.hole.x1));zc.raw(",");zc.dec(uint32_t(p.hole.y1));
            zc.raw("],\"layer_sets\":");zc.dec(ls.sets);zc.raw(",\"layer_detaches\":");zc.dec(ls.detaches);zc.raw(",\"layer_retires\":");zc.dec(ls.retires);
            zc.raw("}\n");DebugChannelSend(zc.t);
        }
    }
    {   // compositor frame cost and escalations (zeroed with the window)
        const CompositorFrameStats cs=CompositorReadFrameStats(zero);
        nvdisp::NvDisplayDriver<LiveBackend>::Line c;
        auto hist=[&](const char* k,const uint32_t (&h)[6]){c.raw(",\"");c.raw(k);c.raw("\":[");for(uint32_t i=0;i<6;++i){if(i)c.raw(",");c.dec(h[i]);}c.raw("]");};
        c.raw("{\"nv_display\":\"stats_frame\",\"loops\":");c.dec(cs.loops);
        c.raw(",\"compose_us\":");c.dec(cs.composeNs/1000);c.raw(",\"present_us\":");c.dec(cs.presentNs/1000);
        c.raw(",\"compose_max\":");c.dec(cs.composeMaxUs);c.raw(",\"present_max\":");c.dec(cs.presentMaxUs);c.raw(",\"loop_max\":");c.dec(cs.loopMaxUs);
        hist("compose",cs.composeHist);hist("present",cs.presentHist);hist("loop",cs.loopHist);
        c.raw(",\"damage_mpx\":");c.dec(cs.damagePixels/1000000);c.raw("}\n");DebugChannelSend(c.t);
        nvdisp::NvDisplayDriver<LiveBackend>::Line e;
        e.raw("{\"nv_display\":\"stats_escalate\",\"flips\":");e.dec(cs.flips);e.raw(",\"full_flips\":");e.dec(cs.fullFlips);
        e.raw(",\"all_marks\":");e.dec(cs.allMarks);e.raw(",\"force\":");e.dec(cs.fullForce);e.raw(",\"requested\":");e.dec(cs.fullRequested);
        e.raw(",\"global\":");e.dec(cs.fullGlobal);e.raw(",\"gpu\":");e.dec(cs.fullGpu);e.raw(",\"hover_full\":");e.dec(cs.fullHover);
        e.raw(",\"unexplained\":");e.dec(cs.sceneUnexplained);e.raw(",\"scene_local\":");e.dec(cs.sceneLocalized);
        e.raw(",\"shell_local\":");e.dec(cs.shellLocalized);e.raw(",\"hover_local\":");e.dec(cs.hoverLocalized);e.raw("}\n");DebugChannelSend(e.t);
    }
    nvdisp::NvDisplayDriver<LiveBackend>::Line l;
    {nvdisp::NvDisplayDriver<LiveBackend>::Line t;t.raw("{\"nv_display\":\"stats_loop\",\"wake\":[");
        for(uint32_t i=0;i<6;++i){if(i)t.raw(",");t.dec(g_wakeHist[i]);}t.raw("],\"arm_line\":[");
        for(uint32_t i=0;i<8;++i){if(i)t.raw(",");t.dec(g_armLineHist[i]);}t.raw("],\"busy\":[");
        for(uint32_t i=0;i<5;++i){if(i)t.raw(",");t.dec(g_busyHist[i]);}t.raw("]}\n");DebugChannelSend(t.t);}
    l.raw("{\"nv_display\":\"stats_poll\",\"us\":");l.dec(us);l.raw(",\"polls\":");l.dec(now.polls-a.polls);
    l.raw(",\"poll_busy\":");l.dec(g_pollBusy);l.raw(",\"poll_cpu_us\":");l.dec((now.pollNs-a.pollNs)/1000);l.raw(",\"poll_max_us\":");l.dec(g_pollMaxUs);l.raw("}\n");
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
    if(ok){NvLayerSetRetireHook(NvLayerRetireLocked);CompositorSetPrimaryPresenter(&g_presenterOps);}
    else{KPrintf("nv-display: bring-up failed; the desktop stays on the GOP framebuffer\n");return;}
    // D3: the second monitor (conn2) joins unless /etc/nv-no-second-head exists. The head-1
    // preflight reads the sink's DPCD first and refuses without one; DP training takes
    // ~1-2 s, during which head 0 presents are deferred (try-lock busy).
    if(!FileExists("/etc/nv-no-second-head")){
        SpinLockAcquire(lock);
        const bool on1=Presenter1OnLocked();
        SpinLockRelease(lock);
        if(on1)Head1Publish();
        else KPrintf("nv-display: head 1 bring-up failed or no sink; the desktop stays on head 0\n");
    }
#if defined(BROOK_NV_GSP)
    NvGspAutostart();   // GSP-RM (p4 keep-display) + CE present, unless /etc/nv-gsp-no-autostart
#endif
}

} // namespace
// 27 Nov NVK head freeze diagnostics: display state snapshots (window ARM/status, LOADV, the
// last ring kick read back through BAR1 and through PRAMIN, BAR1's PD3[0] in VRAM). nv-gsp
// registers the PRAMIN reader and the PD3[0] location once its BAR1 identity map is committed.
namespace { uint32_t (*g_snapPramin)(uint64_t)=nullptr;uint64_t g_snapPd3=0,g_snapPd3Want=0;uint32_t g_snapSeq=0; }
void NvDispSetBar1Diag(uint32_t (*pramin)(uint64_t),uint64_t pd3Addr,uint64_t pd3Want){g_snapPramin=pramin;g_snapPd3=pd3Addr;g_snapPd3Want=pd3Want;}
void NvDispSnapLocked(const char* why){
    if(!g_runnersBuilt||!g_configured||!g_backend.ok)return;
    const uint32_t seq=++g_snapSeq;
    uint32_t pd3lo=0,pd3hi=0;
    if(g_snapPramin&&g_snapPd3){pd3lo=g_snapPramin(g_snapPd3);pd3hi=g_snapPramin(g_snapPd3+4);}
    KPrintf("{\"nv_snap\":\"%s\",\"seq\":%u,\"core_st\":\"%x\",\"exc_win\":\"%x\",\"exc_other\":\"%x\",\"pd3\":\"%x%08x\",\"pd3_ok\":%d,\"b1blk\":\"%x\"}\n",
            why,seq,g_backend.rd(nvdisp::RegChnStatus(nvdisp::kChidCore)),g_backend.rd(nvdisp::kRegExcWin),g_backend.rd(nvdisp::kRegExcOther),
            pd3hi,pd3lo,g_snapPd3?((((uint64_t(pd3hi)<<32)|pd3lo)==g_snapPd3Want)?1:0):-1,g_backend.rd(0xb80f40));
    for(uint32_t o=0;o<nvdisp::kDriverOutputs;++o){
        auto& f=g_driver.f[o];
        if(f.state==nvdisp::FlipState::Off)continue;
        const uint32_t ch=nvdisp::ChidWndw(nvdisp::kDriverWindow[o]);
        uint32_t same=0,n=f.lastKickN>32?32:f.lastKickN,b1=0,pm=0;bool diff=false;
        for(uint32_t i=0;i<n&&g_snapPramin;++i){
            const uint64_t a=f.ring+f.lastKickOff+i*4;const uint32_t x=g_backend.vrd(a),y=g_snapPramin(a);
            if(x==y)++same;else if(!diff){diff=true;b1=x;pm=y;}
        }
        const uint32_t want=f.state==nvdisp::FlipState::Armed?f.armedTarget:(f.front>=0?uint32_t(f.surf[f.front].vram>>8):0);
        KPrintf("{\"nv_snap\":%u,\"o\":%u,\"flip\":%u,\"get\":\"%x\",\"put\":\"%x\",\"st\":\"%x\",\"exc\":\"%x\",\"off\":\"%x\",\"want\":\"%x\",\"iso\":\"%x\"}\n",
                seq,o,uint32_t(f.state),g_backend.rd(nvdisp::RegGet(ch)),f.put,g_backend.rd(nvdisp::RegChnStatus(ch)),g_backend.rd(nvdisp::RegExcept(ch)),
                g_backend.rd(nvdisp::RegArm(ch,nvdisp::c57e::SET_OFFSET0)),want,g_backend.rd(nvdisp::RegArm(ch,nvdisp::c57e::SET_CONTEXT_DMA_ISO0)));
        KPrintf("{\"nv_snap_r\":%u,\"o\":%u,\"lv\":\"%x\",\"rg\":\"%x\",\"kick\":\"%x\",\"ring\":\"%u/%u\",\"b1\":\"%x\",\"pm\":\"%x\"}\n",
                seq,o,g_backend.rd(nvdisp::RegLoadvCounter(f.head)),g_backend.rd(nvdisp::RegRgPos(f.head))&0xffff,f.lastKickOff,same,n,b1,pm);
    }
}
void NvDispSnap(const char* why){SpinLock* lock=NvAuxSharedLock();SpinLockAcquire(lock);NvDispSnapLocked(why);SpinLockRelease(lock);}
namespace { void SnapOnFail(uint32_t){NvDispSnapLocked("flip-failed");} }
void NvDispSetCePresent(const NvCeOps* ops){if(!ops&&g_presenter[0].ceInflight)(void)CeFinishLocked(kCeWaitUs);g_ceOps=ops;}

static bool NvGlassReadyLocked(){
    return g_ceOps && !g_ceVerifyOff && !g_glassUncertainSource &&
           g_presenter[0].active && g_driver.Live(0) &&
           !g_presenter[1].active && !__atomic_load_n(&g_head1Rect,__ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&g_head0Fence,__ATOMIC_ACQUIRE);
}

bool NvGlassAvailable(){
    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock))return false;
    const bool ready=NvGlassReadyLocked();
    SpinLockRelease(lock);
    return ready;
}

int64_t NvGlassPresent(void* prime,uint32_t width,uint32_t height,uint32_t stride){
    if(!width || !height || width>8192 || height>8192 ||
       stride<width*4 || stride>8192*4 || (stride&3))return -22;
    const uint64_t extent=uint64_t(height-1)*stride+width*4;
    uint64_t video=0;
    if(!NvPrimeLinearVram(prime,extent,&video) || !video)return -22;

    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock))return -11;
    const int64_t result=[&]() -> int64_t {
        auto& p=g_presenter[0];
        auto& f=g_driver.f[0];
        if(!NvGlassReadyLocked())return -95;
        if(width!=p.width || height!=p.height)return -116;
        g_driver.Tick();
        if(!g_driver.Live(0))return -95;
        if(p.ceInflight && !CeFinishLocked(kCeWaitUs))return -11;
        if(!NvGlassReadyLocked())return -95;
        const int8_t bb=f.Back();
        if(bb<0)return -11;
        NvCeJob job{};
        job.width=width;job.height=height;
        job.dst_vram=p.vram[bb];job.dst_pitch=p.pitchPx*4;
        job.video_src=video;job.video_pitch=stride;
        uint64_t bytes=0;uint32_t ticket=0;
        const int kicked=g_ceOps->kick(job,&bytes,&ticket);
        if(kicked<=0){
            if(kicked<0)g_ceOps=nullptr;
            return kicked<0?-5:-95;
        }

        // Keep the caller's BO stable until the CE's WFI semaphore. On
        // timeout never flip the candidate or hand it to the CPU presenter.
        // The source reference also keeps the backing out of NVK's allocator.
        ++p.ceKicks;
        const uint64_t w0=PresentNowNs();
        if(g_ceOps->wait(ticket,kCeWaitUs)!=1){
            NvPrimeRef(prime);
            g_glassUncertainSource=prime;
            g_glassUncertainDestination=bb;
            g_ceOps=nullptr;
            p.needFull=true;
            KPrintf("nv-display: Glass CE completion uncertain; quarantining its BOs\n");
            return -5;
        }
        p.ceWaitNs+=PresentNowNs()-w0;
        ++p.ceFrames;p.ceBytes+=bytes;

        // The CPU shadow and any pending damage describe the old producer.
        // A lease loss must rebuild a complete CPU frame into a safe buffer.
        p.shadowValid=false;p.needFull=true;
        p.pending=DamageRects{};p.hole=DamageRect{};p.holeFlipped=false;
        if(f.Present(bb)<0)return -5;
        ++p.frames;++p.glassFrames;p.glassBytes+=bytes;NoteFrame(p);
        return 0;
    }();
    SpinLockRelease(lock);
    return result;
}

bool NvGlassReadFront(uint32_t* pixels,uint32_t width,uint32_t height){
    if(!pixels || !width || !height)return false;
    SpinLock* lock=NvAuxSharedLock();
    if(!SpinLockTryAcquire(lock))return false;
    const bool copied=[&]{
        auto& p=g_presenter[0];
        if(!NvGlassReadyLocked() || width!=p.width || height!=p.height)return false;
        g_driver.Tick();
        const int8_t front=g_driver.f[0].front;
        if(!g_driver.Live(0) || front<0 || front>=int8_t(p.nbuf) || !p.px[front])return false;
        const uint32_t* src=p.px[front];
        for(uint32_t y=0;y<height;++y){
            __builtin_memcpy(pixels+uint64_t(y)*width,src+uint64_t(y)*p.pitchPx,uint64_t(width)*4);
        }
        return true;
    }();
    SpinLockRelease(lock);
    return copied;
}

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
        // `driver desktop on|off|on1|off1|status|stats [reset]|anim on|off`: the compositor presents into
        // head 0's flips (D2) and head 1's as a second output (D3). `off` takes both down.
        if(strncmp(v,"desktop",7)==0){v+=7;while(*v==' ')++v;
            if(strncmp(v,"on1",3)==0){const bool ok=Presenter1OnLocked();SpinLockRelease(lock);if(ok)Head1Publish();PresenterStatus();return true;}
            if(strncmp(v,"off1",4)==0){Presenter1OffLocked();SpinLockRelease(lock);Head1Unpublish();PresenterStatus();return true;}
            if(strncmp(v,"on",2)==0){const bool ok=PresenterOnLocked();SpinLockRelease(lock);if(ok){NvLayerSetRetireHook(NvLayerRetireLocked);CompositorSetPrimaryPresenter(&g_presenterOps);}PresenterStatus();return true;}
            if(strncmp(v,"off",3)==0){
                if(g_presenter[1].active)Presenter1OffLocked();
                g_presenter[0].active=false;SpinLockRelease(lock);Head1Unpublish();CompositorSetPrimaryPresenter(nullptr);
                SpinLockAcquire(lock);g_driver.out0External=false;(void)g_driver.Down0();SpinLockRelease(lock);PresenterStatus();return true;}
            // `anim on|off`: the compositor's pacing probe (a moving square on every output).
            if(strncmp(v,"anim",4)==0){v+=4;while(*v==' ')++v;SpinLockRelease(lock);CompositorSetPacingProbe(strncmp(v,"on",2)==0);
                DebugChannelSend(CompositorPacingProbe()?"{\"nv_display\":\"anim\",\"on\":1}\n":"{\"nv_display\":\"anim\",\"on\":0}\n");return true;}
            // `drag <slot> on|off`: the compositor's drag probe (window slot from `bridge ui windows`, id-1).
            if(strncmp(v,"drag",4)==0){v+=4;while(*v==' ')++v;uint32_t slot=0;bool num=false;while(*v>='0'&&*v<='9'){slot=slot*10+uint32_t(*v-'0');++v;num=true;}
                while(*v==' ')++v;const bool on=strncmp(v,"on",2)==0;SpinLockRelease(lock);
                const bool ok=num&&slot<64&&CompositorSetDragProbe(int(slot),on);
                DebugChannelSend(ok?"{\"nv_display\":\"drag\",\"ok\":1}\n":"{\"nv_display\":\"drag\",\"ok\":0}\n");return true;}
            if(strncmp(v,"stats",5)==0){v+=5;while(*v==' ')++v;PresenterStats(strncmp(v,"reset",5)==0);SpinLockRelease(lock);return true;}
            PresenterStatus();SpinLockRelease(lock);return true;}
        if(!headLive&&!g_driver.Live(1)&&!ConfigureHead()){SpinLockRelease(lock);return true;}
        g_driver.Command(rest+6);
        SpinLockRelease(lock);return true;
    }
    if(strncmp(rest,"direct",6)==0&&(rest[6]==0||rest[6]==' ')){   // zero-copy step 2 A/B: direct on|off
        const char* v=rest+6;while(*v==' ')++v;
        if(!strncmp(v,"off",3))g_directOff=true;else if(!strncmp(v,"on",2))g_directOff=false;
        SpinLockRelease(lock);
        DebugChannelSend(g_directOff?"{\"nv_disp\":\"direct\",\"enabled\":false}\n":"{\"nv_disp\":\"direct\",\"enabled\":true}\n");
        return true;}
    if(strncmp(rest,"snap",4)==0){NvDispSnapLocked("bridge");SpinLockRelease(lock);DebugChannelSend("{\"nv_disp\":\"snap\",\"msg\":\"see the kernel log (nv_snap lines)\"}\n");return true;}
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
NvHead0Arm NvDispHead0Arm(){
    NvHead0Arm a{};
    SpinLock* lock=NvAuxSharedLock();SpinLockAcquire(lock);
    a.live=g_driver.Live(0);a.state=uint32_t(g_driver.f[0].state);
    if(g_configured){
        const uint32_t ch=nvdisp::ChidWndw(nvdisp::kDriverWindow[0]);
        a.iso=g_backend.rd(nvdisp::RegArm(ch,nvdisp::c57e::SET_CONTEXT_DMA_ISO0));
        a.offset=g_backend.rd(nvdisp::RegArm(ch,nvdisp::c57e::SET_OFFSET0));
    }
    SpinLockRelease(lock);
    return a;
}
NvHead0Arm NvDispHead0Fence(bool on,const char* why){
    if(on){
        __atomic_store_n(&g_head0Fence,true,__ATOMIC_RELEASE);
        // let an armed flip land (the presenter's Tick moves it to Idle) before RM work starts
        for(int i=0;i<100;++i){
            SpinLock* lock=NvAuxSharedLock();SpinLockAcquire(lock);
            if(g_driver.Live(0))g_driver.Tick();
            const bool armed=g_driver.Live(0)&&g_driver.f[0].state==nvdisp::FlipState::Armed;
            SpinLockRelease(lock);
            if(!armed)break;
            SchedulerSleepMs(2);
        }
    }
    const NvHead0Arm a=NvDispHead0Arm();
    KPrintf("{\"nv_driver\":\"fence\",\"on\":%d,\"why\":\"%s\",\"live\":%d,\"flip\":%u,\"arm_iso\":\"%x\",\"arm_offset\":\"%x\"}\n",
            on?1:0,why,a.live?1:0,a.state,a.iso,a.offset);
    if(!on)__atomic_store_n(&g_head0Fence,false,__ATOMIC_RELEASE);
    return a;
}
void NvDisplayDesktopStart(){
    // No /etc/nv-display (builder without --nv-display): Brook leaves the display engine alone
    // (GOP framebuffer stays as the firmware set it), but GSP-RM still comes up headless (plain
    // p4, no keep-display, no CE present) - the 27 Nov "hands-off display" A/B.
    if(!MarkerSet("/etc/nv-display")){
#if defined(BROOK_NV_GSP)
        NvGspAutostartHeadless();
#endif
        return;
    }
    Process* t=KernelThreadCreate("nv_display",DesktopStartThread,nullptr,3);
    if(t)SchedulerAddProcess(t);else KPrintf("nv-display: thread start failed\n");
}
} // namespace brook
