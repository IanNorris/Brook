// RTX 2080 display step 3: the gated live path for nv_disp_runner.h.
// `bridge hardware nv-disp plan|preflight|noop|flip|continue|abort|status`.
// Off by default: nothing here runs at boot, and the only GPU accesses are
// made from these explicit bridge commands (and the pre-reset quiesce hook,
// which only acts while a run is live). BAR0 registers and the BAR1 VRAM
// pages named by the plan are mapped uncached on demand; the run holds the
// nv-aux lock (serialised with nv-aux and dp-idle) and busy-waits on the TSC.
#include "nv_disp_live.h"
#include "nv_disp_runner.h"
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
#include <serial.h>

namespace brook {
namespace {

// Small cache of single-page UC mappings (BAR0 registers, BAR1 VRAM pages).
struct PageCache{
    static constexpr uint32_t kSlots=24;
    VirtualAddress va[kSlots]{};uint64_t phys[kSlots]{};uint32_t next=0;
    volatile uint32_t* Map(uint64_t address){
        const uint64_t page=address&~0xfffull;
        for(uint32_t i=0;i<kSlots;++i)if(va[i]&&phys[i]==page)return reinterpret_cast<volatile uint32_t*>(va[i].raw()+(address&0xfff));
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

struct LiveBackend{
    uint64_t bar0=0,bar1=0,bar1Size=0;bool ok=false,toSerial=false;
    PageCache regs{},vram{};
    uint32_t rd(uint32_t a){volatile uint32_t* p=regs.Map(bar0+a);return p?*p:0xffffffffu;}
    void wr(uint32_t a,uint32_t v){volatile uint32_t* p=regs.Map(bar0+a);if(p)*p=v;}
    uint32_t vrd(uint64_t a){if(a+4>bar1Size)return 0xffffffffu;volatile uint32_t* p=vram.Map(bar1+a);return p?*p:0xffffffffu;}
    void vwr(uint64_t a,uint32_t v){if(a+4>bar1Size)return;volatile uint32_t* p=vram.Map(bar1+a);if(p)*p=v;}
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
};

[[clang::require_constant_initialization]] LiveBackend g_backend{};
[[clang::require_constant_initialization]] nvdisp::Runner<LiveBackend> g_runner{};
bool g_configured=false;

void NvDispQuiesceForReset(){
    // Pre-reset: abort a live run (the runner does nothing otherwise). Try the
    // lock briefly; a command stuck mid-run must not block the reset.
    SpinLock* lock=NvAuxSharedLock();bool held=false;
    const uint64_t end=g_backend.nowUs()+100000;
    while(!(held=SpinLockTryAcquire(lock))&&g_backend.nowUs()<end)__asm__ volatile("pause");
    g_backend.toSerial=true;
    g_runner.Quiesce();
    g_backend.toSerial=false;
    if(held)SpinLockRelease(lock);
}
void LiveBackend::hookQuiesce(){(void)PlatformPowerAddQuiesce(NvDispQuiesceForReset);}

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
} // namespace

bool NvDispHandle(const char* command){
    static const char prefix[]="bridge hardware nv-disp";
    const uint32_t n=sizeof(prefix)-1;
    if(strncmp(command,prefix,n)!=0||(command[n]&&command[n]!=' '))return false;
    SpinLock* lock=NvAuxSharedLock();
    SpinLockAcquire(lock);
    // Configure lazily (PCI config reads + a few window-state reads) unless a run is live.
    if(!g_configured||(!g_runner.st.live&&!g_runner.st.paused)){if(!Configure()){SpinLockRelease(lock);return true;}}
    g_runner.Command(command+n);
    SpinLockRelease(lock);
    return true;
}
} // namespace brook
