#include "ahci_readonly.h"
#include "ahci_observe.h"
#include "ahci_read_plan.h"
#include "ahci_readonly_sequence.h"
#include "ahci_firmware_handoff.h"
#include "ahci_write_plan.h"
#include "ssd_trim_plan.h"
#include "ahci_byte_write.h"
#include "ahci_panic_gate.h"
#include "ahci_write_target.h"
#include "ahci_completion.h"
#include "storage_io_stats.h"
#include "ata_identify.h"
#include "pci.h"
#include "vfs.h"
#include "device.h"
#include "sync/kmutex.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/address.h"
#include "apic.h"
#include "idt.h"
#include "irq_wrapper.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "tsc_clock.h"
#include "smp.h"
#include "serial.h"
#include "string.h"
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
#include "fw_cfg.h"
#include "panic_journal.h"
#include "panic_qr.h"
#include "acpi.h"
#include "portio.h"
#endif
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
AhciPanicGate panicGate{};
// A failed gate admission is possible after panic ownership. Never wait for
// that owner or touch the shared DMA buffer in this case.
struct NormalIoGuard {
    bool admitted;
    NormalIoGuard() { KMutexLock(&ioLock); admitted=panicGate.EnterNormal(); }
    ~NormalIoGuard() { if(admitted)panicGate.LeaveNormal(); KMutexUnlock(&ioLock); }
};
bool published=false;
uint16_t trimPayloadLimit=0;
bool writeEnabled=false; // immutable after registration, even if quarantined
bool exportEnabled=false; // immutable boot-only opt-in
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
bool panicTestReady=false,panicTestFat=false,panicIoMode=false;
bool panicReservationRequested=false;
#if defined(BROOK_AHCI_PANIC_TEST)
bool panicTestCandidate=false;
#endif
constexpr uint64_t panicTestStart=4096;
uint64_t panicTestMap[PanicJournalSectors]{};
#endif
#if defined(BROOK_SSD_INSTALLER)
bool physicalInstallMode=false;
bool physicalBlockToolsMode=false;
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
bool testWritable=false;
bool testInstallMode=false;
bool blockToolsMode=false;
bool largeDataTestWritable=false;
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
bool TestKvm() {
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x40000000u),"c"(0));
    return b==0x4b4d564bu && c==0x564b4d56u && d==0x0000004du;
}
#endif
#if defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
bool contentionTestEnabled=false;
uint32_t contentionTestWriter=0;
uint32_t contentionTestYielded=0;
#endif
static constexpr uint32_t pages=17, dataOffset=4096, tableOffset=1280, fisOffset=1024;

// Runtime completion interrupts (single-message MSI to the BSP). PxIE is
// nonzero only while a normal-I/O owner sleeps for its command; the panic
// writer only runs after that owner has left the gate, so it always finds
// port interrupts masked and keeps its polling loop. Interrupt mode can only
// be turned off after boot (automatic miss detection), never back on.
constexpr uint8_t AhciMsiVector=64; // outside the shared INTx pool (48-63)
// Spin budget before sleeping. QEMU: 100 us kept write throughput while
// cutting CPU per byte ~16%; 20 us cut ~35% but lost ~10% throughput.
// Tunable at runtime (bridge hardware ahci-spin-us) for physical measurement.
constexpr uint64_t AhciDefaultSpinNs=100000, AhciMaxSpinNs=2000000;
uint64_t spinNs=AhciDefaultSpinNs;
IrqSpinLock irqLock{};
Process* irqWaiter=nullptr;       // under irqLock
uint64_t irqWakeNs=0;             // under irqLock: when the ISR woke irqWaiter
// True from arming to disarm in SleepForCompletion. In that window the owner
// touches only PxIE, HBA IS and irqLock, never descriptors or the DMA buffer;
// panic capture uses it to recover the gate from a halted or unschedulable
// sleeper (the MSI and timed wakeups are both serviced by the BSP).
bool ownerSleeping=false;
bool irqMode=false;               // published before first normal I/O; cleared on misses
AhciIrqHealth irqHealth{};        // under ioLock (normal I/O owner)
AhciIoStats ioStats{};
AhciIrqStatus irqStatus{};
void StatAdd(uint64_t& field,uint64_t value=1){__atomic_fetch_add(&field,value,__ATOMIC_RELAXED);}
void StatMax(uint64_t& field,uint64_t value){
    uint64_t seen=__atomic_load_n(&field,__ATOMIC_RELAXED);
    while(value>seen && !__atomic_compare_exchange_n(&field,&seen,value,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
bool PanicIo() {
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
    return panicIoMode;
#else
    return false;
#endif
}
bool InterruptsEnabled(){uint64_t f;__asm__ volatile("pushfq; popq %0":"=r"(f)::"memory");return f&0x200;}

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
    // Runtime (non-panic) command completion: spin briefly, then sleep for the
    // completion interrupt or yield. Same tick deadline as the polling loop.
    bool waitCommand(uint64_t timeoutTicks);
    bool waitClear(uint32_t offset,uint32_t mask) {
        if(offset==CI && mask==1u && !PanicIo())return waitCommand(2000);
        const uint64_t start=ApicTickCount();
        const uint32_t limit=
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
            panicIoMode?10000000u:
#endif
            100000000u;
        for(uint32_t spins=0;spins<limit;++spins) {
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
        if(offset==CI && mask==1u && !PanicIo())return waitCommand(30000);
        const uint64_t start=ApicTickCount();
        const uint64_t limit=
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
            panicIoMode?10000000ull:
#endif
            2000000000ull;
        for(uint64_t spins=0;spins<limit;++spins) {
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
// MSI handler (BSP). Masks the port and acknowledges HBA IS.IPS without
// touching PxIS, which the waiter still inspects for completion status.
void AhciIrqBody() {
    StatAdd(ioStats.interrupts);
    if(hba && portRegs) {
        const uint32_t bit=1u<<status.port;
        if(hba[2]&bit){portRegs[IE/4]=0;hba[2]=bit;}
    }
    const uint64_t flags=IrqSpinLockAcquire(&irqLock);
    bool woke=false;
    if(Process* waiter=irqWaiter){irqWaiter=nullptr;irqWakeNs=ElapsedClockNs();SchedulerUnblock(waiter);woke=true;}
    IrqSpinLockRelease(&irqLock,flags);
    // SchedulerUnblock kicks only other idle CPUs. If this CPU was idle, the
    // reschedule IPI (taken after this EOI) dispatches the waiter here now.
    if(woke)ApicSendRescheduleIpi(ApicGetId());
}
enum class SleepOutcome : uint8_t { Interrupted, TimedOut, Skipped };
// Arm, enable port interrupts, recheck, block for at most one tick, disarm.
// The ISR wakes the waiter under irqLock, so after disarm no stale wake can
// reach a later, unrelated sleep of this thread.
SleepOutcome SleepForCompletion(PortIo& port) {
    Process* self=ProcessCurrent();
    __atomic_store_n(&ownerSleeping,true,__ATOMIC_SEQ_CST);
    uint64_t flags=IrqSpinLockAcquire(&irqLock);irqWaiter=self;IrqSpinLockRelease(&irqLock,flags);
    port.write(IE,AhciPortIrqMask);port.fence();
    // A completion just before IE was set may not interrupt; never sleep on it.
    const uint32_t ci=port.read(CI);
    bool block=true;
    if(ci==0xffffffffu || !(ci&1u)) {
        flags=IrqSpinLockAcquire(&irqLock);
        if(irqWaiter==self){irqWaiter=nullptr;block=false;} // no wake was issued
        IrqSpinLockRelease(&irqLock,flags);
    }
    // When the ISR already consumed the arming, pendingWakeup makes this
    // block return at once, consuming that wake here.
    const uint64_t blockedNs=ElapsedClockNs();
    if(block){self->wakeupTick=ApicTickCount()+1;SchedulerBlock(self);}
    const uint64_t resumedNs=ElapsedClockNs();
    flags=IrqSpinLockAcquire(&irqLock);
    const bool interrupted=irqWaiter!=self;
    if(!interrupted)irqWaiter=nullptr;
    const uint64_t wokeNs=irqWakeNs;
    IrqSpinLockRelease(&irqLock,flags);
    if(interrupted && block && wokeNs>=blockedNs && resumedNs>=wokeNs) {
        StatAdd(ioStats.irqLatencyNs,wokeNs-blockedNs);StatAdd(ioStats.wakeLatencyNs,resumedNs-wokeNs);
        StatMax(ioStats.maxWakeLatencyNs,resumedNs-wokeNs);
    }
    // If the block timed out just before the ISR's wake, that wake left a
    // latch for this (now running) thread. Drop it, as KMutexLock does before
    // each block, so it cannot shorten an unrelated later sleep.
    if(interrupted && block)__atomic_store_n(&self->pendingWakeup,0,__ATOMIC_RELEASE);
    port.write(IE,0);hba[2]=1u<<status.port;port.fence(); // mask, clear HBA IS.IPS
    __atomic_store_n(&ownerSleeping,false,__ATOMIC_SEQ_CST);
    if(!block)return SleepOutcome::Skipped;
    return interrupted?SleepOutcome::Interrupted:SleepOutcome::TimedOut;
}
bool PortIo::waitCommand(uint64_t timeoutTicks) {
    struct Wait {
        PortIo& port;
        uint32_t ci(){return port.read(CI);}
        uint64_t nowNs(){return ElapsedClockNsResolution()?ElapsedClockNs():0;}
        uint64_t ticks(){return ApicTickCount();}
        void pause(){__asm__ volatile("pause":::"memory");}
        AhciWaitMode mode() {
            if(!InterruptsEnabled() || !SchedulerIsRunning() || !ProcessCurrent() || panicGate.PanicPending())
                return AhciWaitMode::Spin;
            // A latched error never completes through an interrupt rearm.
            if(port.read(IS)&AhciPortIrqErrors)return AhciWaitMode::Yield;
            return __atomic_load_n(&irqMode,__ATOMIC_ACQUIRE)?AhciWaitMode::Sleep:AhciWaitMode::Yield;
        }
        void sleep() {
            const auto outcome=SleepForCompletion(port);
            if(outcome==SleepOutcome::Interrupted)StatAdd(ioStats.irqWakes);
            else if(outcome==SleepOutcome::TimedOut)StatAdd(ioStats.irqTimeouts);
            else StatAdd(ioStats.irqSkipped);
            const uint32_t now=port.read(CI);
            const bool completed=now!=0xffffffffu && !(now&1u);
            if(outcome!=SleepOutcome::Skipped &&
               !irqHealth.Record(outcome==SleepOutcome::Interrupted,outcome==SleepOutcome::TimedOut,completed) &&
               __atomic_exchange_n(&irqMode,false,__ATOMIC_ACQ_REL)) {
                StatAdd(ioStats.irqDisabled);
                SerialPuts("ahci: completion interrupts missed; reverting to polled completion\n");
            }
        }
        void yield(){SchedulerYield();}
    } wait{*this};
    const uint64_t began=ElapsedClockNs();
    AhciWaitTrace trace;
    // Same iteration caps as the polling loop, for a stalled tick clock.
    const auto result=AhciWaitCommand(wait,timeoutTicks,__atomic_load_n(&spinNs,__ATOMIC_RELAXED),timeoutTicks>2000?2000000000ull:100000000ull,trace);
    const uint64_t ended=ElapsedClockNs();
    StatAdd(ioStats.waits);
    if(!trace.spunOut)StatAdd(ioStats.spinCompletions);
    if(trace.sleeps)StatAdd(ioStats.sleepingWaits);
    StatAdd(ioStats.sleeps,trace.sleeps);StatAdd(ioStats.yields,trace.yields);
    if(ended>=began){StatAdd(ioStats.waitNs,ended-began);StatMax(ioStats.maxWaitNs,ended-began);}
    return result==AhciWaitResult::Complete;
}
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
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
    // Let the normal owner unwind after its current command instead of
    // starting another one while panic waits for its DMA buffer. This is a
    // clean refusal before descriptor changes, not a failed device command.
    if(!panicIoMode && panicGate.PanicPending())return false;
#endif
    bool valid=(plan.fis[2]==0xec || plan.fis[2]==0x25) && plan.commandFlags==5 &&
        plan.prdtEntries==1 && expected && plan.byteCountMinusOne+1==expected;
    const bool isFlush=writeEnabled && plan.fis[2]==0xea && plan.commandFlags==5 &&
        !plan.prdtEntries && !expected;
    valid=valid || isFlush || (writeEnabled && plan.fis[2]==0x35 &&
        plan.commandFlags==(5u|(1u<<6)) && plan.prdtEntries==1 && expected && plan.byteCountMinusOne+1==expected);
    const bool isTrim=writeEnabled && trimPayloadLimit && plan.fis[2]==0x06 &&
        plan.fis[3]==1 && plan.commandFlags==(5u|(1u<<6)) && plan.prdtEntries==1 &&
        expected && expected%512==0 && expected/512<=trimPayloadLimit &&
        expected/512==(uint32_t(plan.fis[12]) | (uint32_t(plan.fis[13]) << 8)) &&
        plan.byteCountMinusOne+1==expected;
    if(!dma || status.quarantined || !(valid || isTrim))
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
    if(!isTrim)StorageIoAccount(uint32_t(StorageIoSource::Ahci),isFlush?2:plan.fis[2]==0x35?1:0,expected);
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
    NormalIoGuard guard;
    if(!guard.admitted || !status.ready)return -5;
    const uint64_t capacity=status.sectors*512;
    if(offset>=capacity)return 0;
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
        NormalIoGuard guard;if(!guard.admitted)return -5;
        AhciByteIo bytes;
        const int result=status.ready ? AhciByteWrite(bytes,offset,data,len,status.sectors,dma+dataOffset) : -5;
#if defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
        constexpr char tag[]="BROOK_QEMU_AHCI_CONTENTION_20261105";
        const auto* input=static_cast<const uint8_t*>(data);
        bool found=false;
        if(contentionTestEnabled && result>0 && input && len>=sizeof(tag)-1)
            for(uint64_t i=0;i<=len-(sizeof(tag)-1);++i)
                if(!memcmp(input+i,tag,sizeof(tag)-1)){found=true;break;}
        uint32_t expected=0;
        if(found && __atomic_compare_exchange_n(&contentionTestWriter,&expected,2,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            SerialPuts("PANIC_CONTENTION_WRITER_HELD\n");
            __atomic_store_n(&contentionTestWriter,1,__ATOMIC_RELEASE);
            for(uint64_t spin=0;spin<2000000000ull && !panicGate.NormalWithPanicPending();++spin)
                __asm__ volatile("pause");
            // Disposable test build only: model a command that takes half a
            // second to finish after the panic asks for the gate.
            const auto& timer=AcpiGetPmTimerInfo();
            if(panicGate.PanicPending() && timer.available) {
                const uint32_t start=inl(timer.port)&0xffffffu;
                for(uint32_t spin=0;spin<20000000;++spin)
                    if(((inl(timer.port)-start)&0xffffffu)>=1789773u) {
                        SerialPanicPuts("PANIC_CONTENTION_DELAY_COMPLETE\n");break;
                    }
            }
            // Exercise the real command boundary while still owning the
            // ordinary gate. Pending panic must reject this next command
            // without quarantining the controller or touching descriptors.
            if(panicGate.PanicPending() && !ReadBlocks(0,1) && status.ready && !status.quarantined)
                __atomic_store_n(&contentionTestYielded,1,__ATOMIC_RELEASE);
        }
#endif
        return result;
    }
    return -30;
}
int Ioctl(Device*,uint32_t request,void*) {
    if(request==DEVICE_IOCTL_READ_ONLY)return writeEnabled?0:1;
    if(request==DEVICE_IOCTL_FLUSH && writeEnabled) {
        NormalIoGuard guard;if(!guard.admitted)return -5;
        AhciReadPlan plan{};
        const bool ok=status.ready && AhciPlanFlush(plan,true) && Run(plan,0);
        return ok?0:-5;
    }
    if(request==DEVICE_IOCTL_FLUSH)return 0;
    return -95;
}
uint64_t BlockCount(Device*) {return status.sectors;}
uint32_t BlockSize(Device*) {return 512;}
const BlockDeviceOps ops={Read,Write,Ioctl,nullptr,BlockCount,BlockSize};
Device device{reinterpret_cast<const DeviceOps*>(&ops),"ahci0",DeviceType::Block,nullptr};
void InstallCompletionIrq();
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
} // namespace brook

extern "C" void AhciMsiIsrInner() { brook::AhciIrqBody(); brook::ApicSendEoi(); }
IRQ_NAKED_HANDLER(AhciMsiIsr, AhciMsiIsrInner)

namespace brook {
namespace {
// Boot-only, on the BSP, before publication. Any failure leaves the previous
// polled completion path (spin, then yield) without changing device state
// beyond the MSI capability. /etc/ahci-polled opts out entirely.
void InstallCompletionIrq() {
    irqStatus.reason="no MSI capability";
    if(auto* marker=VfsOpen("/etc/ahci-polled")){VfsClose(marker);irqStatus.reason="disabled by /etc/ahci-polled";return;}
    const auto b=pci.bus,d=pci.dev,f=pci.fn;
    uint8_t cap=PciConfigRead8(b,d,f,0x34);
    for(unsigned hops=0;cap>=0x40 && cap<=0xfc && !(cap&3) && hops<48;++hops) {
        if(PciConfigRead8(b,d,f,cap)==5) {
            const uint16_t control=PciConfigRead16(b,d,f,cap+2);
            PciMsiLayout layout;
            if(control==0xffffu || !PciMsiRegisters(cap,control,layout)){irqStatus.reason="MSI capability invalid";return;}
            const uint8_t apic=ApicGetId();
            // Keep MSI disabled while programming address/data.
            PciConfigWrite16(b,d,f,cap+2,uint16_t(control&~1u));
            IdtInstallHandler(AhciMsiVector,reinterpret_cast<void*>(AhciMsiIsr));
            PciConfigWrite32(b,d,f,cap+4,0xfee00000u|(uint32_t(apic)<<12));
            if(layout.upper)PciConfigWrite32(b,d,f,layout.upper,0);
            PciConfigWrite16(b,d,f,layout.data,AhciMsiVector);
            if(layout.mask)PciConfigWrite32(b,d,f,layout.mask,PciConfigRead32(b,d,f,layout.mask)&~1u);
            // INTx disable: only the MSI message may signal completion.
            PciCommand(uint16_t(PciConfigRead16(b,d,f,4)|(1u<<10)));
            PciConfigWrite16(b,d,f,cap+2,layout.enabled);
            if(!(PciConfigRead16(b,d,f,cap+2)&1u)){irqStatus.reason="MSI enable not retained";return;}
            // Port interrupts stay masked (PxIE=0) except during a runtime sleep.
            io.write(IE,0);hba[2]=1u<<status.port;
            hba[1]=hba[1]|2u; // GHC.IE
            irqStatus={true,true,AhciMsiVector,apic,"MSI completion interrupts"};
            __atomic_store_n(&irqMode,true,__ATOMIC_RELEASE);
            SerialPrintf("ahci: MSI completion vector=%u LAPIC=%u\n",AhciMsiVector,apic);
            return;
        }
        cap=PciConfigRead8(b,d,f,cap+1);
    }
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
    trimPayloadLimit=AtaTrimPayloadLimit(dma+dataOffset,512);
    status.sectors=info.sectors;status.logicalSectorBytes=info.logicalSectorBytes;
    status.physicalSectorBytes=info.physicalSectorBytes;memcpy(status.model,info.model,sizeof(status.model));
    memcpy(status.serial,info.serial,sizeof(status.serial));
    AhciWriteTarget target{};bool targetPresent=false;
    const bool parsed=ReadWriteTarget(target,targetPresent);
    writeEnabled=parsed && AhciWriteTargetMatches(target,pci.vendorId,pci.deviceId,pci.bus,pci.dev,pci.fn,
        selected,info,AtaSupportsFlushExt(dma+dataOffset,512));
#if defined(BROOK_AHCI_PANIC_TEST)
    // This special build has no production write admission. It requires an
    // explicit QEMU fw_cfg marker and a separate exact-identity scratch disk.
    char testMarker[16]{};
    const auto markerBytes=FwCfgReadFile("opt/brook/panic-test-disk",testMarker,sizeof(testMarker));
    const bool q35=writeEnabled &&
        pci.vendorId==0x8086 && pci.deviceId==0x2922 && pci.bus==0 && pci.dev==31 && pci.fn==2 &&
        selected==0 && info.logicalSectorBytes==512 && info.physicalSectorBytes==512 &&
        !strcmp(info.model,"QEMU HARDDISK");
    panicTestFat=q35 && markerBytes==13 && !memcmp(testMarker,"journal-fat-1",13) &&
        info.sectors==262144 && !strcmp(info.serial,"BROOK-PANIC-FAT");
    panicTestCandidate=panicTestFat || (q35 && markerBytes==9 && !memcmp(testMarker,"journal-1",9) &&
        info.sectors==131072 && !strcmp(info.serial,"BROOK-PANIC-QA"));
    writeEnabled=panicTestCandidate;
#endif
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
    // Never fall back to production admission in this test binary. The
    // harness additionally uses only a fresh regular-file copy as the disk.
    auto* pendingMarker=VfsOpen("/etc/guard-pending-test",VFS_O_STREAM);
    VnodeStat pendingStat{};char pendingValue[2]{};uint64_t pendingOffset=0;
    const bool pendingEnabled=pendingMarker && !VfsStat(pendingMarker,&pendingStat) &&
        !pendingStat.isDir && !pendingStat.isSymlink && pendingStat.size==2 &&
        VfsRead(pendingMarker,pendingValue,2,&pendingOffset)==2 && pendingValue[0]=='1' && pendingValue[1]=='\n';
    if(pendingMarker)VfsRelease(pendingMarker);
    writeEnabled=writeEnabled && pendingEnabled && TestKvm() && pci.vendorId==0x8086 && pci.deviceId==0x2922 &&
        pci.bus==0 && pci.dev==31 && pci.fn==2 && selected==0 && info.sectors==262144 &&
        info.logicalSectorBytes==512 && info.physicalSectorBytes==512 &&
        !strcmp(info.model,"QEMU HARDDISK") && !strcmp(info.serial,"BROOK-RECOVERY-QA");
#endif
#if defined(BROOK_SSD_INSTALLER)
    // Require an exact boot marker and no system-volume selector; reserve the
    // admitted target exclusively for the chosen installer mode, outside the registry.
    auto* installModeMarker=VfsOpen("/etc/ssd-install-mode",VFS_O_STREAM);
    VnodeStat ms{},selector{};char mv[2]{};uint64_t mo=0;
    const bool enabled=installModeMarker && !VfsStat(installModeMarker,&ms) && !ms.isDir && !ms.isSymlink && ms.size==2 &&
        VfsRead(installModeMarker,mv,2,&mo)==2 && mv[0]=='1' && mv[1]=='\n' &&
        VfsLstatPath("/etc/ahci-boot-volume",&selector)==-2 &&
        VfsLstatPath("/etc/ahci-data-volume",&selector)==-2;
    if(installModeMarker)VfsRelease(installModeMarker);
    VnodeStat toolsStat{};const int toolsResult=VfsLstatPath("/etc/ssd-block-tools",&toolsStat);
    const bool toolsRequested=toolsResult!=-2;
    bool toolsValid=false;
    if(!toolsResult && !toolsStat.isDir && !toolsStat.isSymlink && toolsStat.size==2) {
        auto* toolsNode=VfsOpen("/etc/ssd-block-tools",VFS_O_STREAM);
        char value[2]{};uint64_t offset=0;
        toolsValid=toolsNode && VfsRead(toolsNode,value,2,&offset)==2 && value[0]=='1' && value[1]=='\n';
        if(toolsNode)VfsRelease(toolsNode);
    }
    physicalBlockToolsMode=enabled && writeEnabled && toolsRequested && toolsValid;
    physicalInstallMode=enabled && writeEnabled && !toolsRequested;
    writeEnabled=physicalInstallMode || physicalBlockToolsMode;
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    // Test hooks also require the exact disposable QEMU identity. They can be
    // enabled by the test marker or an already matched production policy.
    auto* testMarker=VfsOpen("/etc/ahci-qemu-write-test");
    const bool markerPresent=testMarker!=nullptr;if(testMarker)VfsClose(testMarker);
    auto* installMarker=VfsOpen("/etc/ahci-qemu-install-test");
    const bool installPresent=installMarker!=nullptr;if(installMarker)VfsClose(installMarker);
    auto* largeMarker=VfsOpen("/etc/ahci-qemu-large-data-test");
    const bool largePresent=largeMarker!=nullptr;if(largeMarker)VfsClose(largeMarker);
    auto* toolsMarker=VfsOpen("/etc/ahci-qemu-block-tools-test");
    const bool toolsPresent=toolsMarker!=nullptr;if(toolsMarker)VfsClose(toolsMarker);
    const bool conflicting=largePresent && (markerPresent || installPresent || toolsPresent);
    const bool flush=AtaSupportsFlushExt(dma+dataOffset,512);
    if(toolsPresent) {
        blockToolsMode=!markerPresent && !installPresent && !largePresent &&
            (AhciDisposableTestAdmission(true,TestKvm(),selected,info,flush) ||
             AhciDisposableDesktopInstallAdmission(true,TestKvm(),selected,info,flush));
        writeEnabled=blockToolsMode;
    } else if(largePresent) {
        largeDataTestWritable=AhciDisposableLargeDataAdmission(true,conflicting,TestKvm(),selected,info,flush);
        // A present large-test marker never falls back to production or legacy
        // test admission in this test kernel, even if the identity is wrong.
        writeEnabled=largeDataTestWritable;
    } else {
        testWritable=AhciDisposableTestAdmission(markerPresent || writeEnabled,TestKvm(),selected,info,flush) ||
            AhciDisposableDesktopInstallAdmission(installPresent,TestKvm(),selected,info,flush);
        writeEnabled=writeEnabled || testWritable;
    }
    if(testWritable)testInstallMode=installPresent;
#endif
    KMutexInit(&ioLock);
#if defined(BROOK_SSD_INSTALLER)
    if(!physicalInstallMode && !physicalBlockToolsMode && !DeviceRegister(&device)){Quarantine("AHCI block registration failed");return;}
#else
    if(!DeviceRegister(&device)){Quarantine("AHCI block registration failed");return;}
#endif
    status.ready=true;status.reason="read-only ATA device ready";
#if defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
    char contentionMarker[8]{};
    contentionTestEnabled=TestKvm() && pci.vendorId==0x8086 && pci.deviceId==0x2922 &&
        FwCfgReadFile("opt/brook/panic-contention",contentionMarker,sizeof(contentionMarker))==6 &&
        !memcmp(contentionMarker,"test-1",6);
#endif
#if defined(BROOK_AHCI_PANIC_TEST)
    if(panicTestCandidate && !panicTestFat) {
        // A separate 64 MiB unpartitioned disk is required. Accept an empty
        // journal or a previously valid test capture, never unknown contents.
        bool safe=ReadBlocks(0,1);
        if(safe)for(uint32_t i=0;i<512;++i)if(dma[dataOffset+i]){safe=false;break;}
        struct BootRead {
            bool read(uint32_t sector,uint8_t* output) {
                if(sector>=PanicJournalSectors || !ReadBlocks(panicTestStart+sector,1))return false;
                memcpy(output,dma+dataOffset,512);return true;
            }
        } bootRead;
        PanicJournalRecord a{},b{};
        if(safe)safe=PanicJournalInspect(bootRead,0,a)==PanicJournalResult::Ok &&
                     PanicJournalInspect(bootRead,1,b)==PanicJournalResult::Ok &&
                     PanicJournalWritable(a,b);
        panicTestReady=safe && status.ready;
        SerialPuts(panicTestReady?"PANIC_TEST_DISK_READY\n":"PANIC_TEST_DISK_REJECTED\n");
        char busyMarker[2]{};
        if(panicTestReady && FwCfgReadFile("opt/brook/panic-test-busy",busyMarker,sizeof(busyMarker))==1 &&
           busyMarker[0]=='1' && panicGate.EnterNormal())
            SerialPuts("PANIC_TEST_DISK_BUSY_ARMED\n");
    }
#endif
    if(targetPresent)status.reason=writeEnabled?"configured ATA device writable":"ATA write target rejected; device read-only";
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(testWritable)status.reason="disposable QEMU test ATA device writable";
    if(largeDataTestWritable)status.reason="disposable QEMU large data test ATA device writable";
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
    if(status.ready && !status.quarantined)InstallCompletionIrq();
    __atomic_store_n(&published,true,__ATOMIC_RELEASE);
}
AhciIoStats AhciReadOnlyIoStats() {
    AhciIoStats s;
    auto load=[](const uint64_t& v){return __atomic_load_n(&v,__ATOMIC_RELAXED);};
    s.waits=load(ioStats.waits);s.spinCompletions=load(ioStats.spinCompletions);
    s.sleepingWaits=load(ioStats.sleepingWaits);s.sleeps=load(ioStats.sleeps);s.yields=load(ioStats.yields);
    s.irqWakes=load(ioStats.irqWakes);s.irqTimeouts=load(ioStats.irqTimeouts);s.irqSkipped=load(ioStats.irqSkipped);
    s.irqDisabled=load(ioStats.irqDisabled);s.interrupts=load(ioStats.interrupts);
    s.waitNs=load(ioStats.waitNs);s.maxWaitNs=load(ioStats.maxWaitNs);
    s.irqLatencyNs=load(ioStats.irqLatencyNs);s.wakeLatencyNs=load(ioStats.wakeLatencyNs);
    s.maxWakeLatencyNs=load(ioStats.maxWakeLatencyNs);
    return s;
}
bool AhciReadOnlySetSpinUs(uint64_t us) {
    if(us>AhciMaxSpinNs/1000)return false;
    __atomic_store_n(&spinNs,us*1000,__ATOMIC_RELAXED);return true;
}
uint64_t AhciReadOnlySpinUs(){return __atomic_load_n(&spinNs,__ATOMIC_RELAXED)/1000;}
AhciIrqStatus AhciReadOnlyIrqStatus() {
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE))return {};
    AhciIrqStatus copy=irqStatus;copy.active=__atomic_load_n(&irqMode,__ATOMIC_ACQUIRE);
    if(copy.configured && !copy.active)copy.reason="reverted to polling after missed interrupts";
    return copy;
}
AhciReadOnlyStatus AhciReadOnlyGetStatus() {
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE))return status;
    KMutexLock(&ioLock);const auto copy=status;KMutexUnlock(&ioLock);return copy;
}
uint16_t AhciTrimPayloadLimit(Device* parent) {
    if(parent!=&device || !__atomic_load_n(&published,__ATOMIC_ACQUIRE) || !writeEnabled)
        return 0;
    return trimPayloadLimit;
}

int AhciTrimBatch(Device* parent, TrimRange partition, const TrimRange* ranges,
                  uint32_t count, bool submit, uint32_t& commands) {
    const uint16_t limit=AhciTrimPayloadLimit(parent);
    if(!limit)return -95;
    if(!ranges || !count || !TrimRangeInside(partition,{0,status.sectors}))return -22;
    // Deliberately cap this path to one payload sector per command. This
    // bounds lock hold time and works with any positive IDENTIFY limit.
    uint8_t payload[512]{};
    int error=-22;
    const bool ok=TrimVisitPayloads(ranges,count,partition,nullptr,0,payload,sizeof(payload),limit,
        [&](uint16_t blocks) {
            if(submit) {
                NormalIoGuard guard;
                AhciReadPlan plan{};
                if(!guard.admitted || !status.ready ||
                   !AhciPlanTrim(plan,blocks,limit,dmaPhys+dataOffset,dma64)) {
                    error=-5;
                    return false;
                }
                memcpy(dma+dataOffset,payload,sizeof(payload));
                if(!Run(plan,sizeof(payload))) {
                    error=-5;
                    return false;
                }
            }
            ++commands;
            return true;
        });
    return ok?0:error;
}

Device* AhciWriteAdmittedDevice() {
    if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
#if defined(BROOK_SSD_INSTALLER)
    if(physicalInstallMode || physicalBlockToolsMode)return nullptr;
#endif
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(testInstallMode || blockToolsMode)return nullptr; // reserve test target from boot mounts
#endif
    KMutexLock(&ioLock);Device* result=writeEnabled && status.ready?&device:nullptr;KMutexUnlock(&ioLock);
    return result;
}
#if defined(BROOK_SSD_INSTALLER)
Device* AhciPhysicalInstallDevice() {
    if(!physicalInstallMode || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
    KMutexLock(&ioLock);Device* result=writeEnabled && status.ready?&device:nullptr;KMutexUnlock(&ioLock);return result;
}
Device* AhciPhysicalBlockToolsDevice() {
    if(!physicalBlockToolsMode || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
    KMutexLock(&ioLock);Device* result=writeEnabled && status.ready?&device:nullptr;KMutexUnlock(&ioLock);return result;
}
int AhciPhysicalBlockToolsFinalSync() {
    if(!physicalBlockToolsMode)return 0;
    Device* target=AhciPhysicalBlockToolsDevice();
    const int result=target ? DeviceFlush(target) : -5;
    SerialPrintf("ahci: installer block tools final flush result=%d\n",result);
    return result;
}
#endif
int AhciReadOnlySector(uint64_t lba,void* output) {
    if(!output || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return -22;
    NormalIoGuard guard;
    if(!guard.admitted)return -5;
    if(!status.ready || lba>=status.sectors)return -22;
    const bool ok=ReadBlocks(lba,1);
    if(ok)memcpy(output,dma+dataOffset,512);
    return ok?512:-5;
}
#if defined(BROOK_AHCI_PANIC_TEST) || defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_AUTO_RECOVERY)
bool AhciPanicTestFatCandidate() {
    return panicTestFat && __atomic_load_n(&published,__ATOMIC_ACQUIRE);
}
bool AhciPanicTestMapSectors(const uint64_t* sectors,uint32_t count,uint64_t first,uint64_t length) {
    if(!panicTestFat || panicTestReady || !sectors || count!=PanicJournalSectors ||
       !__atomic_load_n(&published,__ATOMIC_ACQUIRE) || !length ||
       first>=status.sectors || length>status.sectors-first)return false;
    NormalIoGuard guard;if(!guard.admitted || !status.ready)return false;
    uint64_t mapped[PanicJournalSectors]{};
    for(uint32_t i=0;i<count;++i) {
        if(sectors[i]>=length)return false;
        mapped[i]=first+sectors[i];
        for(uint32_t j=0;j<i;++j)if(mapped[i]==mapped[j])return false;
    }
    struct MapRead {
        const uint64_t* map;
        bool read(uint32_t sector,uint8_t* output) {
            if(sector>=PanicJournalSectors || !ReadBlocks(map[sector],1))return false;
            memcpy(output,dma+dataOffset,512);return true;
        }
    } reader{mapped};
    PanicJournalRecord a{},b{};
    if(PanicJournalInspect(reader,0,a)!=PanicJournalResult::Ok ||
       PanicJournalInspect(reader,1,b)!=PanicJournalResult::Ok ||
       !PanicJournalWritable(a,b))return false;
    for(uint32_t i=0;i<count;++i)panicTestMap[i]=mapped[i];
    panicTestReady=true;return true;
}
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
bool AhciGuardPanicTestMapSectors(const uint64_t* sectors,uint32_t count,uint64_t first,uint64_t length) {
    // The guard test binary already admits only the exact disposable KVM disk.
    // A second explicit selector is required before arming panic writes.
    char marker[8]{};
    if(!writeEnabled || !TestKvm() || FwCfgReadFile("opt/guard-panic-capture",marker,sizeof(marker))!=6 ||
       memcmp(marker,"test-1",6) || panicTestFat || panicTestReady)return false;
    panicTestFat=true;
    if(AhciPanicTestMapSectors(sectors,count,first,length))return true;
    panicTestFat=false;return false;
}
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool AhciGuardPanicRecoveryMapSectors(const uint64_t* sectors,uint32_t count,uint64_t first,uint64_t length) {
    if(!writeEnabled || panicTestFat || panicTestReady)return false;
    panicTestFat=true;
    if(AhciPanicTestMapSectors(sectors,count,first,length))return true;
    panicTestFat=false;return false;
}
#endif
void AhciPanicReserveBeforeHalt() {
    if(!panicTestReady)return;
    const auto request=panicGate.RequestPanic();
    if(request==AhciPanicGate::Request::Unavailable)return;
    panicReservationRequested=true;
    if(request==AhciPanicGate::Request::Pending) {
        // Peers are still running here. Do not block forever if the current
        // CPU or a failed peer owns normal I/O; never seize its DMA buffer.
        SerialPanicPuts("PANIC_DISK_RESERVE_WAIT\n");
        const auto& timer=AcpiGetPmTimerInfo();
        if(timer.available) {
            struct WaitIo {
                uint16_t port;
                uint32_t readTimer(){return inl(port);}
                void pause(){__asm__ volatile("pause":::"memory");}
            } wait{timer.port};
            (void)AhciWaitForPanicOwner(panicGate,wait,20000000);
        } else {
            for(uint32_t spins=0;spins<10000000 && !panicGate.PanicOwned();++spins)
                __asm__ volatile("pause");
        }
        SerialPanicPuts(panicGate.PanicOwned()?"PANIC_DISK_RESERVE_READY\n":"PANIC_DISK_RESERVE_HELD\n");
#if defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
        if(panicGate.PanicOwned() && __atomic_load_n(&contentionTestYielded,__ATOMIC_ACQUIRE))
            SerialPanicPuts("PANIC_CONTENTION_NEXT_COMMAND_YIELDED\n");
#endif
    }
}
#if defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
void AhciPanicTestWaitForWriter() {
    if(!contentionTestEnabled)return;
    for(uint32_t spin=0;spin<200000000 && __atomic_load_n(&contentionTestWriter,__ATOMIC_ACQUIRE)!=1;++spin)
        __asm__ volatile("pause");
}
#endif
// After SmpHaltAllAPs. A pending normal owner parked in its completion sleep
// cannot finish when the BSP (its MSI and timed-wakeup CPU) panicked with
// interrupts off. Once every peer CPU is confirmed halted, that owner can never
// run again; take the gate only if the port is idle (no DMA in flight).
static void RecoverGateFromParkedOwner() {
    if(!panicReservationRequested || panicGate.PanicOwned() || !panicGate.PanicPending() ||
       !__atomic_load_n(&ownerSleeping,__ATOMIC_SEQ_CST))return;
    const uint32_t self=SmpCurrentCpuIndex(),cpus=SmpGetCpuCount();
    for(uint32_t c=0;c<cpus;++c) {
        if(c==self)continue;
        const auto* halted=SmpGetHaltedState(c);
        if(!halted || !halted->halted){SerialPanicPuts("PANIC_DISK_PARKED_OWNER_PEER_RUNNING\n");return;}
    }
    bool idle=false;
    for(uint32_t spin=0;spin<10000000 && !idle;++spin) {
        const uint32_t ci=io.read(CI),sact=io.read(SACT);
        if(ci==0xffffffffu || sact==0xffffffffu)break;
        idle=!ci && !sact;
        if(!idle)__asm__ volatile("pause");
    }
    if(idle && panicGate.TakeFromParkedOwner()) {
        io.write(IE,0); // the parked owner never masked it
        SerialPanicPuts("PANIC_DISK_PARKED_OWNER_TAKEN\n");
    } else SerialPanicPuts("PANIC_DISK_PARKED_OWNER_BUSY\n");
}
void AhciPanicTestCapture() {
    const uint8_t* packet=nullptr;uint32_t bytes=0;
    if(!panicTestReady || !PanicCapturedPacket(&packet,&bytes))return;
    RecoverGateFromParkedOwner();
    if(!(panicReservationRequested?panicGate.PanicOwned():panicGate.EnterPanic())) {
#if defined(BROOK_SSD_AUTO_RECOVERY)
        const char* message="PANIC_DISK_BUSY\n";
#else
        const char* message="PANIC_TEST_DISK_BUSY\n";
#endif
        while(*message)SerialPanicPutChar(*message++);
        return;
    }
    panicIoMode=true;
    struct PanicIo {
        bool read(uint32_t sector,uint8_t* output) {
            if(sector>=PanicJournalSectors || !ReadBlocks(panicTestFat?panicTestMap[sector]:panicTestStart+sector,1))return false;
            memcpy(output,dma+dataOffset,512);return true;
        }
        bool write(uint32_t sector,const uint8_t* input) {
            if(sector>=PanicJournalSectors)return false;
            memcpy(dma+dataOffset,input,512);AhciReadPlan plan{};
            const uint64_t lba=panicTestFat?panicTestMap[sector]:panicTestStart+sector;
            return AhciPlanWrite(plan,lba,1,status.sectors,dmaPhys+dataOffset,dma64) && Run(plan,512);
        }
        bool flush() {
            AhciReadPlan plan{};return AhciPlanFlush(plan,true) && Run(plan,0);
        }
    } panicIo;
    const auto result=PanicJournalWrite(panicIo,packet,bytes);
#if defined(BROOK_SSD_AUTO_RECOVERY)
    const char* message=result==PanicJournalResult::Ok?"PANIC_DISK_CAPTURE_OK\n":"PANIC_DISK_CAPTURE_FAILED\n";
#else
    const char* message=result==PanicJournalResult::Ok?"PANIC_TEST_DISK_CAPTURE_OK\n":"PANIC_TEST_DISK_CAPTURE_FAILED\n";
#endif
    while(*message)SerialPanicPutChar(*message++);
    // Never release panic ownership: other CPUs may not actually have halted.
}
#endif
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
bool AhciDisposableLargeDataWritable() { return largeDataTestWritable && __atomic_load_n(&published,__ATOMIC_ACQUIRE); }
Device* AhciDisposableQemuInstallDevice() {
    if(!testWritable || !testInstallMode || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
    KMutexLock(&ioLock);Device* result=writeEnabled && status.ready?&device:nullptr;KMutexUnlock(&ioLock);return result;
}
Device* AhciDisposableQemuBlockToolsDevice() {
    if(!blockToolsMode || !__atomic_load_n(&published,__ATOMIC_ACQUIRE))return nullptr;
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
