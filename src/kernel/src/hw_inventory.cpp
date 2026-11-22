#include "hw_inventory.h"
#include "hw_inventory_format.h"
#include "acpi.h"
#include "acpi_tables.h"
#include "boot_resource_map.h"
#include "cpu.h"
#include "kprintf.h"
#include "net.h"
#include "pci.h"
#include "serial.h"
#include "smp.h"
#include "vfs.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "../../shared/boot_protocol/boot_protocol.h"
#if defined(BROOK_BRIDGE_V3)
#include "debug_bridge_v3.h"
#endif

namespace brook {
namespace {
constexpr uint32_t Capacity=192*1024;
char g_text[Capacity];
HwText g_out(g_text,Capacity);
bool g_captured=false,g_attempted=false;
uint32_t g_lines=0;

struct Block {uint32_t begin,end;uint16_t vendor;uint8_t cls,sub,bus,dev,fn;};
Block g_blocks[256];uint32_t g_blockCount=0;
uint32_t g_ecamFunctions=0,g_ecamRefused=0;
bool g_dmarPresent=false;
char g_cpuBrand[49];
char g_dmarLine[160];
char g_dmarUnits[160];
uint64_t g_drhdBase[4];uint32_t g_drhdCount=0;

HwMcfgEntry g_mcfg[8];uint32_t g_mcfgCount=0;
uint8_t g_tableCopy[16384];
volatile uint32_t* g_window=nullptr;VirtualAddress g_windowVa{};

void Cpuid(uint32_t leaf,uint32_t sub,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d) {
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(sub));
}

void CpuSection() {
    uint32_t a,b,c,d;Cpuid(0,0,a,b,c,d);const uint32_t maxLeaf=a;
    char vendor[13];
    for(unsigned i=0;i<4;++i){vendor[i]=char(b>>(8*i));vendor[4+i]=char(d>>(8*i));vendor[8+i]=char(c>>(8*i));}
    vendor[12]=0;
    Cpuid(0x80000000u,0,a,b,c,d);const uint32_t maxExt=a;
    g_cpuBrand[0]=0;
    if(maxExt>=0x80000004u) {
        uint32_t* words=reinterpret_cast<uint32_t*>(g_cpuBrand);
        for(uint32_t leaf=0;leaf<3;++leaf){Cpuid(0x80000002u+leaf,0,a,b,c,d);words[leaf*4]=a;words[leaf*4+1]=b;words[leaf*4+2]=c;words[leaf*4+3]=d;}
        g_cpuBrand[48]=0;
    }
    const char* brand=g_cpuBrand;while(*brand==' ')++brand;
    Cpuid(1,0,a,b,c,d);
    const uint32_t signature=a;
    uint32_t family=(a>>8)&15,model=(a>>4)&15;const uint32_t stepping=a&15;
    if(family==15)family+=(a>>20)&0xff;
    if(family==6 || family>=15)model|=((a>>16)&15)<<4;
    const bool hypervisor=(c>>31)&1;
    g_out.str("cpu vendor ");g_out.str(vendor);g_out.str(" brand \"");g_out.str(brand);g_out.str("\"");g_out.end();
    g_out.str("cpu signature ");g_out.hx(signature,8);g_out.str(" family ");g_out.hx(family,2);g_out.str(" model ");
    g_out.hx(model,2);g_out.str(" stepping ");g_out.dec(stepping);g_out.str(" max_leaf ");g_out.hx(maxLeaf);
    g_out.str(hypervisor?" hypervisor":" bare-metal");g_out.end();
    g_out.str("cpu features ecx ");g_out.hx(c,8);g_out.str(" edx ");g_out.hx(d,8);
    if(maxLeaf>=7){uint32_t a7,b7,c7,d7;Cpuid(7,0,a7,b7,c7,d7);g_out.str(" leaf7 ebx ");g_out.hx(b7,8);g_out.str(" ecx ");g_out.hx(c7,8);}
    g_out.end();
    // IA32_BIOS_SIGN_ID: read after CPUID(1) above. The conventional zero
    // write before CPUID is skipped to keep this path free of MSR writes; on
    // current Intel parts firmware-loaded revisions read back without it.
    const bool intel=vendor[0]=='G' && vendor[7]=='I' && vendor[11]=='l';
    if(intel){const uint64_t rev=ReadMsr(0x8b);g_out.str("cpu microcode_rev ");g_out.hx(rev>>32);g_out.str(" (msr 0x8b, no pre-write)");g_out.end();}
    else {g_out.str("cpu microcode_rev unavailable (non-Intel)");g_out.end();}
}

void MemorySection(const BootProtocol* boot) {
    if(!boot || !boot->memoryMap){g_out.str("memmap unavailable\n");return;}
    uint64_t totals[16]{};
    g_out.str("memmap entries ");g_out.dec(boot->memoryMapCount);g_out.end();
    for(uint32_t i=0;i<boot->memoryMapCount;++i) {
        const auto& m=boot->memoryMap[i];const uint32_t type=uint32_t(m.type);
        g_out.str("memmap ");g_out.hx(m.physicalStart,16);g_out.ch('-');g_out.hx(m.physicalStart+m.pageCount*4096-1,16);
        g_out.ch(' ');g_out.str(HwMemoryTypeName(type));g_out.str(" pages ");g_out.dec(m.pageCount);g_out.end();
        if(type<16)totals[type]+=m.pageCount;
    }
    for(uint32_t t=0;t<16;++t) if(totals[t]) {
        g_out.str("memmap total ");g_out.str(HwMemoryTypeName(t));g_out.ch(' ');g_out.size(totals[t]*4096);g_out.end();
    }
    const auto& fb=boot->framebuffer;
    g_out.str("gop framebuffer ");g_out.dec(fb.width);g_out.ch('x');g_out.dec(fb.height);g_out.str(" stride ");g_out.dec(fb.stride);
    g_out.str(" format ");g_out.dec(uint32_t(fb.format));g_out.str(" base ");g_out.hx(fb.physicalBase,16);g_out.end();
}


void AcpiSection() {
    const uint32_t count=AcpiInventoryCount();
    g_out.str("acpi tables ");g_out.dec(count);g_out.end();
    for(uint32_t i=0;i<count;++i) {
        AcpiSdtHeader h{};
        if(!AcpiInventoryHeader(i,h)){g_out.str("acpi [");g_out.dec(i);g_out.str("] unreadable\n");continue;}
        g_out.str("acpi [");g_out.dec(i);g_out.str("] ");g_out.raw(h.signature,4);g_out.str(" len ");g_out.dec(h.length);
        g_out.str(" rev ");g_out.dec(h.revision);g_out.str(" oem \"");g_out.raw(h.oemId,6);g_out.str("\" \"");g_out.raw(h.oemTableId,8);
        g_out.str("\" oemrev ");g_out.hx(h.oemRevision,8);
        // Only table headers are listed; bodies such as MSDM (product key) are never read.
        g_out.end();
    }
    uint32_t bytes=0;
    if(AcpiUniqueTableCopy("MCFG",g_tableCopy,sizeof(g_tableCopy),bytes)) {
        g_mcfgCount=HwParseMcfg(g_tableCopy,bytes,g_mcfg,8);
        for(uint32_t i=0;i<g_mcfgCount;++i) {
            g_out.str("mcfg segment ");g_out.dec(g_mcfg[i].segment);g_out.str(" buses ");g_out.hex(g_mcfg[i].startBus,2);g_out.ch('-');
            g_out.hex(g_mcfg[i].endBus,2);g_out.str(" ecam ");g_out.hx(g_mcfg[i].base,16);g_out.end();
        }
    } else g_out.str("mcfg absent or not unique; extended config space unavailable\n");
    bytes=0;
    const uint32_t start=g_out.length();
    if(AcpiUniqueDmarCopy(g_tableCopy,sizeof(g_tableCopy),bytes)){
        g_dmarPresent=true;HwFormatDmar(g_out,g_tableCopy,bytes);
        for(uint32_t o=48;o+16<=bytes && g_drhdCount<4;) {
            const uint32_t type=uint32_t(HwLe(g_tableCopy+o,2)),length=uint32_t(HwLe(g_tableCopy+o+2,2));
            if(length<4 || o+length>bytes)break;
            if(type==0 && length>=16)g_drhdBase[g_drhdCount++]=HwLe(g_tableCopy+o+8,8);
            o+=length;
        }
    }
    else g_out.str("dmar absent, duplicated or larger than 16KB (VT-d disabled in firmware?)\n");
    uint32_t n=0;for(uint32_t i=start;i<g_out.length() && g_text[i]!='\n' && n+1<sizeof(g_dmarLine);++i)g_dmarLine[n++]=g_text[i];
    g_dmarLine[n]=0;
}

bool EcamPhysical(uint8_t bus,uint8_t dev,uint8_t fn,uint64_t& physical) {
    for(uint32_t i=0;i<g_mcfgCount;++i) {
        const auto& e=g_mcfg[i];
        if(e.segment || bus<e.startBus || bus>e.endBus)continue;
        physical=e.base+(uint64_t(bus-e.startBus)<<20)+(uint64_t(dev)<<15)+(uint64_t(fn)<<12);
        return true;
    }
    return false;
}

// Read-only, uncached, non-executable mapping of exactly one ECAM page.
bool MapEcam(uint64_t physical) {
    if(!g_window) {
        g_windowVa=VmmAllocPagesAccounted(1,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid,PmmOwner::Other);
        if(!g_windowVa)return false;
        const auto leaf=VmmDetachPage(KernelPageTable,g_windowVa);
        if(leaf)PmmFreePage(VmmLeafPhysical(leaf));
        __asm__ volatile("invlpg (%0)"::"r"(g_windowVa.raw()):"memory");
        g_window=reinterpret_cast<volatile uint32_t*>(g_windowVa.raw());
    }
    BootResourceClassification c{};
    constexpr uint32_t allowed=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Mmio));
    if(!AcpiClassifyBootRange(physical,4096,c) || (c.types&~allowed))return false;
    return VmmMapPage(KernelPageTable,g_windowVa,PhysicalAddress(physical),
                      VMM_NO_EXEC|VMM_CACHE_DISABLE|VMM_WRITE_THRU,MemTag::Device,KernelPid);
}
void UnmapEcam() {
    // Borrowed device frame: detach without returning it to the allocator.
    (void)VmmDetachPage(KernelPageTable,g_windowVa);
    __asm__ volatile("invlpg (%0)"::"r"(g_windowVa.raw()):"memory");
}

// VT-d unit state left by firmware: version, capability and global status
// (translation / interrupt remapping enabled?) — register reads only.
void DmarUnitSection() {
    HwText line(g_dmarUnits,sizeof(g_dmarUnits));
    line.str("dmar units ");line.dec(g_drhdCount);
    for(uint32_t i=0;i<g_drhdCount;++i) {
        const uint64_t base=g_drhdBase[i];
        g_out.str("dmar unit ");g_out.dec(i);g_out.str(" base ");g_out.hx(base,8);
        if((base&0xfff) || !MapEcam(base)){g_out.str(" unmapped (not a reserved/MMIO page)");g_out.end();line.str(" ?");continue;}
        const uint32_t version=g_window[0],capLo=g_window[2],capHi=g_window[3],gsts=g_window[0x1c/4];
        UnmapEcam();
        g_out.str(" ver ");g_out.hx(version&0xff,2);g_out.str(" cap ");g_out.hx((uint64_t(capHi)<<32)|capLo,16);
        g_out.str(" gsts ");g_out.hx(gsts,8);
        g_out.str((gsts>>31)&1?" dma-translation ON":" dma-translation off");
        g_out.str((gsts>>25)&1?" irq-remap ON":" irq-remap off");
        g_out.end();
        line.str(" [");line.dec(i);line.str((gsts>>31)&1?" TES=1":" TES=0");line.str((gsts>>25)&1?" IRES=1]":" IRES=0]");
    }
}

void PciSection() {
    uint32_t functions=0;
    for(uint32_t bus=0;bus<256;++bus) for(uint32_t dev=0;dev<32;++dev) {
        const uint32_t id0=PciConfigRead32(uint8_t(bus),uint8_t(dev),0,0);
        if((id0&0xffff)==0xffff)continue;
        const uint32_t fnCount=(PciConfigRead8(uint8_t(bus),uint8_t(dev),0,0x0e)&0x80)?8:1;
        for(uint32_t fn=0;fn<fnCount;++fn) {
            const uint8_t b=uint8_t(bus),d=uint8_t(dev),f=uint8_t(fn);
            const uint32_t id=PciConfigRead32(b,d,f,0);
            if((id&0xffff)==0xffff)continue;
            ++functions;
            uint64_t physical=0;bool extended=false;
            if(EcamPhysical(b,d,f,physical)) {
                extended=MapEcam(physical);
                // ECAM must alias the legacy view; otherwise do not trust it.
                if(extended && g_window[0]!=id){UnmapEcam();extended=false;}
                if(extended)++g_ecamFunctions;else ++g_ecamRefused;
            }
            const uint32_t cls=PciConfigRead32(b,d,f,8);
            const uint32_t begin=g_out.length();
            auto read=[&](uint16_t offset)->uint32_t{
                if(offset>=0x100)return extended?g_window[offset/4]:0xffffffffu;
                return PciConfigRead32(b,d,f,uint8_t(offset));
            };
            HwFormatPciFunction(g_out,b,d,f,read,extended);
            if(extended)UnmapEcam();
            if(g_blockCount<sizeof(g_blocks)/sizeof(g_blocks[0]))
                g_blocks[g_blockCount++]={begin,g_out.length(),uint16_t(id),uint8_t(cls>>24),uint8_t(cls>>16),b,d,f};
        }
    }
    g_out.str("pci functions ");g_out.dec(functions);g_out.str(" ecam_read ");g_out.dec(g_ecamFunctions);
    g_out.str(" ecam_refused ");g_out.dec(g_ecamRefused);g_out.end();
}


// NVIDIA functions: raw configuration space (legacy 256 bytes, then ECAM
// extended space; rows that read all-zero or all-ones are skipped) for each
// 10de function and the bridge above it, then the GPU's own boot registers
// from a read-only uncached map of BAR0's first page. Reads only; no BAR
// sizing, no command register changes, nothing written to the device.
void ConfigRows(uint8_t b,uint8_t d,uint8_t f) {
    uint64_t physical=0;bool extended=false;
    const uint32_t id=PciConfigRead32(b,d,f,0);
    if(EcamPhysical(b,d,f,physical)){extended=MapEcam(physical);if(extended && g_window[0]!=id){UnmapEcam();extended=false;}}
    const uint32_t limit=extended?0x1000:0x100;
    for(uint32_t row=0;row<limit;row+=16) {
        uint32_t v[4];bool zero=true,ones=true;
        for(uint32_t k=0;k<4;++k){
            const uint32_t o=row+k*4;
            v[k]=o<0x100?PciConfigRead32(b,d,f,uint8_t(o)):g_window[o/4];
            zero&=v[k]==0;ones&=v[k]==0xffffffffu;
        }
        if(row>=0x40 && (zero || ones))continue;
        g_out.str("nvcfg ");g_out.hex(b,2);g_out.ch(':');g_out.hex(d,2);g_out.ch('.');g_out.hex(f,1);
        g_out.ch(' ');g_out.hex(row,3);g_out.ch(':');
        for(uint32_t k=0;k<4;++k){g_out.ch(' ');g_out.hex(v[k],8);}
        g_out.end();
    }
    if(extended)UnmapEcam();
    else g_out.str("nvcfg extended space unavailable (no ECAM)\n");
}
void NvidiaSection() {
    for(uint32_t i=0;i<g_blockCount;++i) {
        const auto& blk=g_blocks[i];
        if(blk.vendor!=0x10de)continue;
        if(blk.fn==0) {
            // Bridge whose secondary bus is the GPU's bus.
            for(uint32_t dev=0;dev<32;++dev) for(uint32_t fn=0;fn<8;++fn) {
                const uint32_t id=PciConfigRead32(0,uint8_t(dev),uint8_t(fn),0);
                if((id&0xffff)==0xffff)continue;
                if((PciConfigRead8(0,uint8_t(dev),uint8_t(fn),0x0e)&0x7f)!=1)continue;
                if(PciConfigRead8(0,uint8_t(dev),uint8_t(fn),0x19)!=blk.bus)continue;
                g_out.str("nvidia upstream bridge 00:");g_out.hex(dev,2);g_out.ch('.');g_out.hex(fn,1);
                g_out.str(" id ");g_out.hex(id,8);g_out.end();
                ConfigRows(0,uint8_t(dev),uint8_t(fn));
            }
        }
        ConfigRows(blk.bus,blk.dev,blk.fn);
        if(blk.cls!=0x03)continue;
        const uint16_t command=PciConfigRead16(blk.bus,blk.dev,blk.fn,4);
        const uint32_t bar0=PciConfigRead32(blk.bus,blk.dev,blk.fn,0x10);
        uint64_t base=bar0&~uint64_t(15);
        if((bar0&6)==4)base|=uint64_t(PciConfigRead32(blk.bus,blk.dev,blk.fn,0x14))<<32;
        g_out.str("nvidia bar0 ");g_out.hx(base,8);g_out.str(" command ");g_out.hx(command,4);
        if((bar0&1) || !base || (base&0xfff) || !(command&2)){g_out.str(" not read (I/O, unassigned or memory decode off)");g_out.end();continue;}
        if(!MapEcam(base)){g_out.str(" not read (range is RAM or unmappable)");g_out.end();continue;}
        const uint32_t boot0=g_window[0],boot42=g_window[0xa00/4];
        UnmapEcam();
        g_out.str(" PMC_BOOT_0 ");g_out.hx(boot0,8);g_out.str(" PMC_BOOT_42 ");g_out.hx(boot42,8);
        // Turing: architecture in BOOT_0[28:20] (0x16x = TU10x), implementation [23:20].
        g_out.str(" arch ");g_out.hx((boot0>>20)&0x1ff,3);g_out.str(" rev ");g_out.hx(boot0&0xff,2);
        g_out.end();
    }
}

void CountLines(){g_lines=0;for(uint32_t i=0;i<g_out.length();++i)if(g_text[i]=='\n')++g_lines;}

// Console lines go through KPrintf's 256-byte overlay buffer; copy one line at a time.
void ConsoleLines(uint32_t begin,uint32_t end,uint32_t maxLines) {
    char line[200];uint32_t n=0,lines=0;
    for(uint32_t i=begin;i<end && lines<maxLines;++i) {
        if(g_text[i]=='\n' || n+1==sizeof(line)) {
            line[n]=0;KPrintf("HWINV %s\n",line);n=0;++lines;
            if(g_text[i]=='\n')continue;
        }
        line[n++]=g_text[i];
    }
}
}

bool HwInventoryCaptured(){return g_captured;}
uint32_t HwInventoryBytes(){return g_captured?g_out.length():0;}
uint32_t HwInventoryLines(){return g_captured?g_lines:0;}
bool HwInventoryOverflow(){return g_out.overflow();}
uint32_t HwInventoryRead(uint32_t offset,char* out,uint32_t count) {
    if(!out || !count)return 0;
    out[0]=0;if(!g_captured || offset>=g_out.length())return 0;
    uint32_t n=g_out.length()-offset;if(n>count-1)n=count-1;
    for(uint32_t i=0;i<n;++i)out[i]=g_text[offset+i];
    out[n]=0;return n;
}

void HwInventoryCapture(const BootProtocol* boot) {
    if(g_attempted)return;g_attempted=true;
    auto* marker=VfsOpen("/etc/hw-inventory");if(!marker)return;VfsClose(marker);
    if(SmpCurrentCpuIndex()!=0){SerialPuts("HWINV: skipped (not BSP)\n");return;}
    g_out.str("brook hw-inventory v1 (read-only: no BAR sizing, no VPD, no MSI-X table, ACPI headers only)\n");
    CpuSection();
    MemorySection(boot);
    AcpiSection();
    DmarUnitSection();
    PciSection();
    NvidiaSection();
    if(g_out.overflow())g_out.str("\ninventory truncated\n");
    CountLines();
    g_captured=true;
    SerialPuts("HWINV_BEGIN\n");
    for(uint32_t i=0;i<g_out.length();) {
        char chunk[257];uint32_t n=0;
        while(n<256 && i<g_out.length())chunk[n++]=g_text[i++];
        chunk[n]=0;SerialPuts(chunk);
    }
    SerialPrintf("HWINV_END bytes=%u lines=%u overflow=%u\n",g_out.length(),g_lines,g_out.overflow()?1u:0u);
}

void HwInventoryAnnounce() {
    if(!g_captured)return;
    const char* brand=g_cpuBrand;while(*brand==' ')++brand;
    KPrintf("HWINV CPU %s; %u logical CPUs\n",brand,SmpGetCpuCount());
    for(uint32_t i=0;i<g_blockCount;++i) {
        const auto& b=g_blocks[i];
        const bool nvidia=b.vendor==0x10de;
        const bool key=nvidia || b.cls==0x02 || (b.cls==0x0c && b.sub==0x03) || b.cls==0x01 || b.cls==0x03;
        if(!key)continue;
        ConsoleLines(b.begin,b.end,nvidia?48:1);
    }
    KPrintf("HWINV %s\n",g_dmarLine[0]?g_dmarLine:"dmar summary unavailable");
    if(g_drhdCount)KPrintf("HWINV %s\n",g_dmarUnits);
    KPrintf("HWINV %u bytes / %u lines captured%s; full text: bridge hardware inventory <offset> <count>\n",
            g_out.length(),g_lines,g_out.overflow()?" (TRUNCATED)":"");
    NetIf* nif=NetGetIf();
    if(nif && nif->ipAddr) {
        const uint32_t ip=nif->ipAddr;
        KPrintf("BRIDGE endpoint tcp %u.%u.%u.%u:1234 (v3 operator auth)\n",ip&0xff,(ip>>8)&0xff,(ip>>16)&0xff,ip>>24);
    } else KPrintf("BRIDGE no IPv4 address (no NIC or no DHCP lease); bridge unreachable\n");
#if defined(BROOK_BRIDGE_V3)
    DebugBridgeV3LogBoot();
#else
    KPrintf("BRIDGE built without v3 authentication\n");
#endif
}
}
