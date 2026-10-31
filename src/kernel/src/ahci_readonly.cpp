#include "ahci_readonly.h"
#include "ahci_observe.h"
#include "ahci_read_plan.h"
#include "ahci_readonly_sequence.h"
#include "ahci_firmware_handoff.h"
#include "ahci_write_plan.h"
#include "ahci_byte_write.h"
#include "ahci_write_target.h"
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
bool writeEnabled=false; // immutable after registration, even if quarantined
bool exportEnabled=false; // immutable boot-only opt-in
#if defined(BROOK_SSD_INSTALLER)
bool physicalInstallMode=false;
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
bool testWritable=false;
bool testInstallMode=false;
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
    volatile uint32_t* regs=nullptr;
    uint32_t read(uint32_t offset) { return regs[offset/4]; }
    void write(uint32_t offset,uint32_t value) { regs[offset/4]=value; }
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
} io;
struct HandoffIo {
    PortIo port(uint32_t i) { return PortIo{hba+(0x100+i*0x80)/4}; }
    bool disableInterrupts() {
        const uint32_t ghc=hba[1];if(ghc==0xffffffffu || !(ghc&0x80000000u) || (ghc&1u))return false;
        hba[1]=ghc&~2u;__atomic_thread_fence(__ATOMIC_SEQ_CST);
        return ControllerModeReady(hba[1]);
    }
};

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
    const bool isFlush=writeEnabled && plan.fis[2]==0xea && plan.commandFlags==5 &&
        !plan.prdtEntries && !expected;
    valid=valid || isFlush || (writeEnabled && plan.fis[2]==0x35 &&
        plan.commandFlags==(5u|(1u<<6)) && plan.prdtEntries==1 && expected && plan.byteCountMinusOne+1==expected);
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
    const auto result=isFlush ? SubmitNoData(io) : Submit(io,expected);
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
struct AhciByteIo {
    bool read(uint64_t lba,uint32_t count,uint8_t*) { return ReadBlocks(lba,count); }
    bool write(uint64_t lba,uint32_t count,const uint8_t*) {
        AhciReadPlan p{};return AhciPlanWrite(p,lba,count,status.sectors,dmaPhys+dataOffset,dma64) && Run(p,count*512);
    }
};
int Write(Device*,uint64_t offset,const void* data,uint64_t len) {
    if(writeEnabled && __atomic_load_n(&published,__ATOMIC_ACQUIRE)) {
        KMutexLock(&ioLock);AhciByteIo bytes;
        const int result=status.ready ? AhciByteWrite(bytes,offset,data,len,status.sectors,dma+dataOffset) : -5;
        KMutexUnlock(&ioLock);return result;
    }
    return -30;
}
int Ioctl(Device*,uint32_t request,void*) {
    if(request==DEVICE_IOCTL_READ_ONLY)return writeEnabled?0:1;
    if(request==DEVICE_IOCTL_FLUSH && writeEnabled) {
        KMutexLock(&ioLock);AhciReadPlan plan{};
        const bool ok=status.ready && AhciPlanFlush(plan,true) && Run(plan,0);
        KMutexUnlock(&ioLock);return ok?0:-5;
    }
    if(request==DEVICE_IOCTL_FLUSH)return 0;
    return -95;
}
uint64_t BlockCount(Device*) {return status.sectors;}
uint32_t BlockSize(Device*) {return 512;}
const BlockDeviceOps ops={Read,Write,Ioctl,nullptr,BlockCount,BlockSize};
Device device{reinterpret_cast<const DeviceOps*>(&ops),"ahci0",DeviceType::Block,nullptr};
bool ReadWriteTarget(AhciWriteTarget& target,bool& present) {
    auto* node=VfsOpen("/etc/ahci-write-target",VFS_O_STREAM);
    present=node!=nullptr;if(!node)return false;
    VnodeStat st{};char record[192]{};uint64_t offset=0;
    bool ok=!VfsStat(node,&st) && !st.isDir && !st.isSymlink && st.size && st.size<=sizeof(record);
    if(ok)ok=VfsRead(node,record,st.size,&offset)==int(st.size) &&
        AhciWriteTargetParse(record,uint32_t(st.size),target);
    VfsRelease(node);return ok;
}
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
    // No global or link reset. Require one active link, and stop only idle
    // firmware engines before replacing any DMA pointers.
    uint32_t selected=0,active=0;const uint32_t maxPorts=(cap&31u)+1;
    for(uint32_t i=0;i<maxPorts;++i) if(pi&(1u<<i)) {
        auto* p=hba+(0x100+i*0x80)/4;
        const uint32_t cmd=p[CMD/4],ci=p[CI/4],sact=p[SACT/4],link=p[0x28/4];
        if(cmd==0xffffffffu || ci==0xffffffffu || sact==0xffffffffu || link==0xffffffffu) {
            PciCommand(original);status.reason="AHCI port registers unavailable";return;
        }
        if((link&15u)==3 && ((link>>8)&15u)==1) {selected=i;++active;}
    }
    if(active!=1){PciCommand(original);status.reason="expected one active SATA link";return;}
    HandoffIo handoff;uint32_t failurePort=0;
    const auto stopped=QuiesceFirmware(handoff,pi,maxPorts,failurePort);
    if(stopped!=HandoffResult::Ok) {
        status.port=failurePort;
        status.reason=stopped==HandoffResult::CommandsActive?"AHCI firmware command active":
            stopped==HandoffResult::DeviceBusy?"AHCI firmware device busy":
            stopped==HandoffResult::Interrupts?"AHCI interrupt handoff failed":
            stopped==HandoffResult::StopFailed?"AHCI firmware engine did not stop":"AHCI firmware state invalid";
        return;
    }
    PciCommand(uint16_t((original|2u)&~4u));
    if(PciConfigRead16(pci.bus,pci.dev,pci.fn,4)!=uint16_t((original|2u)&~4u)) {
        PciCommand(original);status.reason="AHCI bus mastering could not be disabled";return;
    }
    status.port=selected;portRegs=hba+(0x100+selected*0x80)/4;io.regs=portRegs;dma64=(cap&(1u<<31))!=0;
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
    memcpy(status.serial,info.serial,sizeof(status.serial));
    AhciWriteTarget target{};bool targetPresent=false;
    const bool parsed=ReadWriteTarget(target,targetPresent);
    writeEnabled=parsed && AhciWriteTargetMatches(target,pci.vendorId,pci.deviceId,pci.bus,pci.dev,pci.fn,
        selected,info,AtaSupportsFlushExt(dma+dataOffset,512));
#if defined(BROOK_SSD_INSTALLER)
    // A physical installer build never exposes a writable general-purpose disk.
    // Require an exact boot marker and no system-volume selector; the admitted
    // target is reserved exclusively for the installer, outside the registry.
    auto* installModeMarker=VfsOpen("/etc/ssd-install-mode",VFS_O_STREAM);
    VnodeStat ms{},selector{};char mv[2]{};uint64_t mo=0;
    const bool enabled=installModeMarker && !VfsStat(installModeMarker,&ms) && !ms.isDir && !ms.isSymlink && ms.size==2 &&
        VfsRead(installModeMarker,mv,2,&mo)==2 && mv[0]=='1' && mv[1]=='\n' &&
        VfsLstatPath("/etc/ahci-boot-volume",&selector)==-2;
    if(installModeMarker)VfsRelease(installModeMarker);
    physicalInstallMode=enabled && writeEnabled;
    writeEnabled=physicalInstallMode;
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    // Test hooks also require the exact disposable QEMU identity. They can be
    // enabled by the test marker or an already matched production policy.
    auto* testMarker=VfsOpen("/etc/ahci-qemu-write-test");
    const bool markerPresent=testMarker!=nullptr;if(testMarker)VfsClose(testMarker);
    auto* installMarker=VfsOpen("/etc/ahci-qemu-install-test");
    const bool installPresent=installMarker!=nullptr;if(installMarker)VfsClose(installMarker);
    testWritable=AhciDisposableTestAdmission(markerPresent || writeEnabled,TestKvm(),selected,info,
        AtaSupportsFlushExt(dma+dataOffset,512)) ||
        AhciDisposableDesktopInstallAdmission(installPresent,TestKvm(),selected,info,
        AtaSupportsFlushExt(dma+dataOffset,512));
    writeEnabled=writeEnabled || testWritable;
    if(testWritable)testInstallMode=installPresent;
#endif
    KMutexInit(&ioLock);
#if defined(BROOK_SSD_INSTALLER)
    if(!physicalInstallMode && !DeviceRegister(&device)){Quarantine("AHCI block registration failed");return;}
#else
    if(!DeviceRegister(&device)){Quarantine("AHCI block registration failed");return;}
#endif
    status.ready=true;status.reason="read-only ATA device ready";
    if(targetPresent)status.reason=writeEnabled?"configured ATA device writable":"ATA write target rejected; device read-only";
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(testWritable)status.reason="disposable QEMU test ATA device writable";
#endif
    if(!writeEnabled) {
        auto* exportMarker=VfsOpen("/etc/ahci-export",VFS_O_STREAM);
        if(exportMarker) {
            VnodeStat st{};char value[2]{};uint64_t offset=0;
            exportEnabled=!VfsStat(exportMarker,&st) && !st.isDir && !st.isSymlink && st.size==2 &&
                VfsRead(exportMarker,value,2,&offset)==2 && value[0]=='1' && value[1]=='\n';
            VfsRelease(exportMarker);
        }
    }
    __atomic_store_n(&published,true,__ATOMIC_RELEASE);
}
AhciReadOnlyStatus AhciReadOnlyGetStatus() {
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE))return status;
    KMutexLock(&ioLock);const auto copy=status;KMutexUnlock(&ioLock);return copy;
}
Device* AhciWriteAdmittedDevice() {
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
#if defined(BROOK_SSD_INSTALLER)
    if(physicalInstallMode)return nullptr;
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(testInstallMode)return nullptr; // reserve the test target from boot mounts
#endif
    KMutexLock(&ioLock);Device* result=writeEnabled && status.ready?&device:nullptr;KMutexUnlock(&ioLock);
    return result;
}
#if defined(BROOK_SSD_INSTALLER)
Device* AhciPhysicalInstallDevice() {
    if(!physicalInstallMode || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
    KMutexLock(&ioLock);Device* result=writeEnabled && status.ready?&device:nullptr;KMutexUnlock(&ioLock);return result;
}
#endif
int AhciReadOnlySector(uint64_t lba,void* output) {
    if(!output || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return -22;
    KMutexLock(&ioLock);
    if(!status.ready || lba>=status.sectors){KMutexUnlock(&ioLock);return -22;}
    const bool ok=ReadBlocks(lba,1);
    if(ok)memcpy(output,dma+dataOffset,512);KMutexUnlock(&ioLock);
    return ok?512:-5;
}
bool AhciReadOnlyExportInfo(BrookAhciExportInfo& info) {
    info={};if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE) || !exportEnabled || writeEnabled)return false;
    KMutexLock(&ioLock);
    const bool ready=status.ready && !status.quarantined;
    if(ready) {
        info.version=BROOK_AHCI_EXPORT_VERSION;info.logical_sector_bytes=status.logicalSectorBytes;
        info.physical_sector_bytes=status.physicalSectorBytes;info.port=status.port;info.sectors=status.sectors;
        memcpy(info.model,status.model,sizeof(info.model));memcpy(info.serial,status.serial,sizeof(info.serial));
    }
    KMutexUnlock(&ioLock);return ready;
}
int AhciReadOnlyExportRead(uint64_t offset,void* output,uint32_t length) {
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE) || !exportEnabled || writeEnabled)return -30;
    if(!output || !length || length>BROOK_AHCI_EXPORT_CHUNK || (length&511) || (offset&511) ||
       offset>status.sectors*512 || length>status.sectors*512-offset)return -22;
    const int result=Read(&device,offset,output,length);
    return result==int(length)?result:-5;
}
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
bool AhciDisposableQemuWritable() { return testWritable && !testInstallMode && __atomic_load_n(&published,__ATOMIC_ACQUIRE); }
Device* AhciDisposableQemuInstallDevice() {
    if(!testWritable || !testInstallMode || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
    KMutexLock(&ioLock);Device* result=writeEnabled && status.ready?&device:nullptr;KMutexUnlock(&ioLock);return result;
}
int AhciDisposableQemuTest() {
    if(!testWritable || testInstallMode)return -30;
    uint8_t bytes[1030];for(uint32_t i=0;i<sizeof(bytes);++i)bytes[i]=uint8_t((i*37u+11u)&255u);
    const int written=DeviceWrite(&device,4096ull*512+7,bytes,sizeof(bytes));
    return written==int(sizeof(bytes)) ? DeviceFlush(&device) : -5;
}
#endif
} // namespace brook
