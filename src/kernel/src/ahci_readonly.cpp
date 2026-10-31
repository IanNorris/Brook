#include "ahci_readonly.h"
#include "ahci_observe.h"
#include "ahci_read_plan.h"
#include "ahci_readonly_sequence.h"
#include "ata_identify.h"
#include "pci.h"
#include "vfs.h"
#include "device.h"
#include "sync/kmutex.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/address.h"
#include "apic.h"
#include "string.h"
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
#include "ahci_write_plan.h"
#include "ahci_byte_write.h"
#include "ahci_test_admission.h"
#endif

namespace brook {
namespace {
using namespace ahci_ro;
AhciReadOnlyStatus status{};
volatile uint32_t* hba=nullptr;
volatile uint32_t* portRegs=nullptr;
uint8_t* dma=nullptr;
uint64_t dmaPhys=0;
bool dma64=false;
PciDevice pci{};
KMutex ioLock{};
bool published=false;
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
bool testWritable=false;
bool TestKvm() {
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x40000000u),"c"(0));
    return b==0x4b4d564bu && c==0x564b4d56u && d==0x0000004du;
}
#endif
static constexpr uint32_t pages=17, dataOffset=4096, tableOffset=1280, fisOffset=1024;

struct CommandHeader {
    uint16_t flags, prdtEntries;
    volatile uint32_t transferred;
    uint32_t tableLow, tableHigh, reserved[4];
};
static_assert(sizeof(CommandHeader)==32,"AHCI command header size");
struct Prdt { uint32_t low, high, reserved, byteCountMinusOne; };
static_assert(sizeof(Prdt)==16,"AHCI PRDT size");

struct PortIo {
    uint32_t read(uint32_t offset) { return portRegs[offset/4]; }
    void write(uint32_t offset,uint32_t value) { portRegs[offset/4]=value; }
    void fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
    uint32_t transferred() {
        auto* hdr=reinterpret_cast<CommandHeader*>(dma);
        return __atomic_load_n(&hdr->transferred,__ATOMIC_ACQUIRE);
    }
    bool waitClear(uint32_t offset,uint32_t mask) {
        const uint64_t start=ApicTickCount();
        for(uint32_t spins=0;spins<100000000u;++spins) {
            const uint32_t value=read(offset);
            if(value==0xffffffffu)return false;
            if(!(value&mask))return true;
            const uint64_t now=ApicTickCount();
            if(now>=start && now-start>=2000)return false;
            __asm__ volatile("pause");
        }
        return false;
    }
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    bool waitClearFlush(uint32_t offset,uint32_t mask) {
        const uint64_t start=ApicTickCount();
        for(uint64_t spins=0;spins<2000000000ull;++spins) {
            const uint32_t value=read(offset);
            if(value==0xffffffffu)return false;
            if(!(value&mask))return true;
            const uint64_t now=ApicTickCount();
            if(now>=start && now-start>=30000)return false;
            __asm__ volatile("pause");
        }
        return false;
    }
#endif
} io;

void PciCommand(uint16_t value) {
    // Avoid echoing adjacent PCI status W1C bits.
    PciConfigWrite32(pci.bus,pci.dev,pci.fn,4,value);
}
void Quarantine(const char* reason) {
    status.ready=false;status.quarantined=true;status.reason=reason;
    // Even on successful stop we deliberately retain every DMA page until
    // reboot. If stop fails, late DMA can never target recycled memory.
    (void)Quiesce(io);
}

bool Run(const AhciReadPlan& plan, uint32_t expected) {
    bool valid=(plan.fis[2]==0xec || plan.fis[2]==0x25) && plan.commandFlags==5 &&
        plan.prdtEntries==1 && expected && plan.byteCountMinusOne+1==expected;
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    const bool isFlush=testWritable && plan.fis[2]==0xea && plan.commandFlags==5 &&
        !plan.prdtEntries && !expected;
    valid=valid || isFlush || (testWritable && plan.fis[2]==0x35 &&
        plan.commandFlags==(5u|(1u<<6)) && plan.prdtEntries==1 && expected && plan.byteCountMinusOne+1==expected);
#endif
    if(!dma || status.quarantined || !valid)
        return false;
    // Do not alter descriptors if hardware unexpectedly owns any slot.
    if (io.read(CI) != 0 || io.read(SACT) != 0) {
        Quarantine("AHCI command slot unexpectedly active");
        return false;
    }
    memset(dma,0,1024);memset(dma+tableOffset,0,256);
    auto* header=reinterpret_cast<CommandHeader*>(dma);
    header->flags=plan.commandFlags;header->prdtEntries=plan.prdtEntries;
    header->tableLow=uint32_t(dmaPhys+tableOffset);header->tableHigh=uint32_t((dmaPhys+tableOffset)>>32);
    memcpy(dma+tableOffset,plan.fis,sizeof(plan.fis));
    if(plan.prdtEntries) {
        auto* prd=reinterpret_cast<Prdt*>(dma+tableOffset+128);
        prd->low=plan.dmaLow;prd->high=plan.dmaHigh;prd->byteCountMinusOne=plan.byteCountMinusOne;
    }
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    const auto result=isFlush ? SubmitNoData(io) : Submit(io,expected);
#else
    const auto result=Submit(io,expected);
#endif
    if(result!=Result::Ok) { Quarantine("AHCI command failed or timed out");return false; }
    const uint8_t* d2h=dma+fisOffset+0x40;
    if(d2h[0]==0x34 && (d2h[2]&1u)) { Quarantine("ATA device reported an error");return false; }
    return true;
}

bool ReadBlocks(uint64_t lba,uint32_t count) {
    AhciReadPlan plan{};
    return status.ready && AhciPlanRead(plan,lba,count,status.sectors,dmaPhys+dataOffset,dma64) && Run(plan,count*512);
}

int Read(Device*,uint64_t offset,void* output,uint64_t length) {
    if(!output && length)return -22;
    if(length>0x7fffffffu)return -22;
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE))return -5;
    KMutexLock(&ioLock);
    if(!status.ready){KMutexUnlock(&ioLock);return -5;}
    const uint64_t capacity=status.sectors*512;
    if(offset>=capacity){KMutexUnlock(&ioLock);return 0;}
    if(length>capacity-offset)length=capacity-offset;
    auto* dst=static_cast<uint8_t*>(output);uint64_t done=0;
    while(done<length) {
        const uint64_t pos=offset+done;const uint32_t in=uint32_t(pos%512);
        const uint64_t remaining=length-done;
        const uint32_t blocks=in || remaining<512 ? 1 : uint32_t(remaining/512>128?128:remaining/512);
        if(!ReadBlocks(pos/512,blocks))break;
        uint64_t bytes=uint64_t(blocks)*512-in;if(bytes>remaining)bytes=remaining;
        memcpy(dst+done,dma+dataOffset+in,bytes);done+=bytes;
    }
    KMutexUnlock(&ioLock);
    return done?int(done):(length?-5:0);
}
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
struct TestByteIo {
    bool read(uint64_t lba,uint32_t count,uint8_t*) { return ReadBlocks(lba,count); }
    bool write(uint64_t lba,uint32_t count,const uint8_t*) {
        AhciReadPlan p{};return AhciPlanWrite(p,lba,count,status.sectors,dmaPhys+dataOffset,dma64) && Run(p,count*512);
    }
};
#endif
int Write(Device*,uint64_t offset,const void* data,uint64_t len) {
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(testWritable && __atomic_load_n(&published,__ATOMIC_ACQUIRE)) {
        KMutexLock(&ioLock);TestByteIo bytes;
        const int result=status.ready ? AhciByteWrite(bytes,offset,data,len,status.sectors,dma+dataOffset) : -5;
        KMutexUnlock(&ioLock);return result;
    }
#else
    (void)offset;(void)data;(void)len;
#endif
    return -30;
}
int Ioctl(Device*,uint32_t request,void*) {
    if(request==DEVICE_IOCTL_READ_ONLY) {
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
        if(testWritable)return 0;
#endif
        return 1;
    }
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(request==DEVICE_IOCTL_FLUSH && testWritable) {
        KMutexLock(&ioLock);AhciReadPlan plan{};
        const bool ok=status.ready && AhciPlanFlush(plan,true) && Run(plan,0);
        KMutexUnlock(&ioLock);return ok?0:-5;
    }
#endif
    if(request==DEVICE_IOCTL_FLUSH)return 0;
    return -95;
}
uint64_t BlockCount(Device*) {return status.sectors;}
uint32_t BlockSize(Device*) {return 512;}
const BlockDeviceOps ops={Read,Write,Ioctl,nullptr,BlockCount,BlockSize};
Device device{reinterpret_cast<const DeviceOps*>(&ops),"ahci0",DeviceType::Block,nullptr};
}

void AhciReadOnlyInit() {
    auto* marker=VfsOpen("/etc/ahci-read-only");if(!marker)return;VfsClose(marker);
    status.requested=true;status.reason="AHCI observation unavailable";
    const auto& observed=AhciObserveRead();hba=AhciObserveMappedPage();
    if(!observed.valid || observed.controllerCount!=1 || !hba)return;
    bool found=false;
    for(uint32_t i=0;i<PciBootInventoryCount();++i) {
        PciBootEntry e{};if(!PciBootInventoryRead(i,&e))continue;
        const auto& d=e.device;
        if(d.bus==observed.bus && d.dev==observed.slot && d.fn==observed.function){pci=d;found=true;break;}
    }
    if(!found){status.reason="AHCI PCI identity changed";return;}
    const uint16_t original=PciConfigRead16(pci.bus,pci.dev,pci.fn,4);
    if(original==0xffffu){status.reason="AHCI PCI command unavailable";return;}
    // Establish MMIO decode without changing bus mastering until every port
    // has been checked for firmware-owned work.
    PciCommand(uint16_t(original|2u));
    if(PciConfigRead16(pci.bus,pci.dev,pci.fn,4)!=uint16_t(original|2u)) {
        PciCommand(original);status.reason="AHCI PCI decode unavailable";return;
    }
    const uint32_t ghc=hba[1],cap=hba[0],pi=hba[3],version=hba[4];
    if(ghc==0xffffffffu || !(ghc&0x80000000u) || (ghc&1u) || !AhciObservePortMap(cap,pi)) {
        PciCommand(original);status.reason="AHCI controller state invalid";return;
    }
    if(version>=0x10200u) {
        const uint32_t cap2=hba[0x24/4];
        if(cap2==0xffffffffu){PciCommand(original);status.reason="AHCI extended capabilities unavailable";return;}
        if(cap2&1u) {
            const uint32_t bohc=hba[0x28/4];
            if(bohc==0xffffffffu || (bohc&0x11u) || !(bohc&2u)) {
                PciCommand(original);status.reason="AHCI firmware ownership not relinquished";return;
            }
        }
    }
    // No global reset or link reset. Refuse all firmware-active engines and
    // require exactly one active link before replacing any DMA pointers.
    uint32_t selected=0,active=0;const uint32_t maxPorts=(cap&31u)+1;
    for(uint32_t i=0;i<maxPorts;++i) if(pi&(1u<<i)) {
        auto* p=hba+(0x100+i*0x80)/4;
        const uint32_t cmd=p[CMD/4],ci=p[CI/4],sact=p[SACT/4],link=p[0x28/4];
        if(cmd==0xffffffffu || ci==0xffffffffu || sact==0xffffffffu || link==0xffffffffu ||
           (cmd&(ST|FRE|CR|FR)) || ci || sact) {
            PciCommand(original);status.reason="AHCI port is not idle";return;
        }
        if((link&15u)==3 && ((link>>8)&15u)==1) {selected=i;++active;}
    }
    if(active!=1){PciCommand(original);status.reason="expected one active SATA link";return;}
    PciCommand(uint16_t((original|2u)&~4u));
    if(PciConfigRead16(pci.bus,pci.dev,pci.fn,4)!=uint16_t((original|2u)&~4u)) {
        PciCommand(original);status.reason="AHCI bus mastering could not be disabled";return;
    }
    status.port=selected;portRegs=hba+(0x100+selected*0x80)/4;dma64=(cap&(1u<<31))!=0;
    const auto phys=PmmAllocPages(pages,MemTag::Device,KernelPid);
    if(!phys){PciCommand(original);status.reason="AHCI DMA allocation failed";return;}
    dmaPhys=phys.raw();
    if(dmaPhys>=(16ull<<30) || uint64_t(pages)*4096>(16ull<<30)-dmaPhys ||
       !DmaRange(dmaPhys,pages*4096,4096,dma64)) {
        for(uint32_t i=0;i<pages;++i)PmmFreePage(phys+uint64_t(i)*4096);
        dmaPhys=0;PciCommand(original);status.reason="AHCI DMA address unsupported";return;
    }
    dma=reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());memset(dma,0,pages*4096);
    if(Prepare(io,dmaPhys,dmaPhys+fisOffset,dma64)!=Result::Ok) {
        status.reason="AHCI port preparation refused";return;
    }
    // All ports are idle and descriptors are owned; disable controller IRQs
    // and select AHCI mode, then enable PCI bus mastering for the single slot.
    hba[1]=(ghc|0x80000000u)&~3u;
    const uint32_t mode=hba[1];
    if(!ControllerModeReady(mode)){status.reason="AHCI mode unavailable";return;}
    PciCommand(uint16_t(original|6u));
    if(PciConfigRead16(pci.bus,pci.dev,pci.fn,4)!=uint16_t(original|6u)) {
        PciCommand(uint16_t((original|2u)&~4u));status.reason="AHCI bus mastering unavailable";return;
    }
    if(Start(io)!=Result::Ok) {Quarantine("AHCI engine start failed");return;}
    if(!io.waitClear(TFD,0x88u)) {Quarantine("AHCI device did not become ready");return;}
    // Signature may only become available after enabling FIS receive (QEMU
    // behaves this way). Do not send ATA commands to non-ATA devices.
    if(io.read(0x24)!=0x00000101u) {Quarantine("AHCI port is not an ATA disk");return;}
    AhciReadPlan plan{};
    if(!AhciPlanIdentify(plan,dmaPhys+dataOffset,dma64) || !Run(plan,512))return;
    AtaIdentifyInfo info{};
    if(AtaParseIdentify(dma+dataOffset,512,info)!=AtaIdentifyResult::Ok ||
       !(AtaIdentifyWord(dma+dataOffset,49)&(1u<<8))) {
        Quarantine("ATA identify geometry or DMA unsupported");return;
    }
    status.sectors=info.sectors;status.logicalSectorBytes=info.logicalSectorBytes;
    status.physicalSectorBytes=info.physicalSectorBytes;memcpy(status.model,info.model,sizeof(status.model));
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    // This build is only for disposable QEMU fixtures: require all conditions,
    // including an otherwise unused per-image marker and exact ATA identity.
    auto* testMarker=VfsOpen("/etc/ahci-qemu-write-test");
    if(testMarker){VfsClose(testMarker);testWritable=AhciDisposableTestAdmission(true,TestKvm(),selected,info,
        AtaSupportsFlushExt(dma+dataOffset,512));}
#endif
    KMutexInit(&ioLock);
    if(!DeviceRegister(&device)){Quarantine("AHCI block registration failed");return;}
    status.ready=true;status.reason="read-only ATA device ready";
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(testWritable)status.reason="disposable QEMU test ATA device writable";
#endif
    __atomic_store_n(&published,true,__ATOMIC_RELEASE);
}
AhciReadOnlyStatus AhciReadOnlyGetStatus() {
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE))return status;
    KMutexLock(&ioLock);const auto copy=status;KMutexUnlock(&ioLock);return copy;
}
int AhciReadOnlySector(uint64_t lba,void* output) {
    if(!output || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return -22;
    KMutexLock(&ioLock);
    if(!status.ready || lba>=status.sectors){KMutexUnlock(&ioLock);return -22;}
    const bool ok=ReadBlocks(lba,1);
    if(ok)memcpy(output,dma+dataOffset,512);KMutexUnlock(&ioLock);
    return ok?512:-5;
}
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
int AhciDisposableQemuTest() {
    if(!testWritable)return -30;
    uint8_t bytes[1030];for(uint32_t i=0;i<sizeof(bytes);++i)bytes[i]=uint8_t((i*37u+11u)&255u);
    const int written=DeviceWrite(&device,4096ull*512+7,bytes,sizeof(bytes));
    return written==int(sizeof(bytes)) ? DeviceFlush(&device) : -5;
}
#endif
} // namespace brook
