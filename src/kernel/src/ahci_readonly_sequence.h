#pragma once
#include <stdint.h>

namespace brook {
// Port-local AHCI offsets. This sequence never constructs an ATA write.
namespace ahci_ro {
static constexpr uint32_t CLB=0x00, CLBU=0x04, FB=0x08, FBU=0x0c, IS=0x10, IE=0x14;
static constexpr uint32_t CMD=0x18, TFD=0x20, SIG=0x24, SSTS=0x28, SCTL=0x2c, SERR=0x30, SACT=0x34, CI=0x38;
static constexpr uint32_t ST=1u, FRE=1u<<4, FR=1u<<14, CR=1u<<15;
static constexpr uint32_t ERROR_MASK=(1u<<30)|(1u<<29)|(1u<<28)|(1u<<27)|(1u<<24);
// AHCI 1.3.1 §§7.1, 7.3.1: CPDS, PCS and SERR.DIAG.X indicate
// possible media replacement. Partial/Slumber also set PRCS and DIAG.N.
static constexpr uint32_t MEDIA_CHANGE_IRQ=(1u<<31)|(1u<<6);
static constexpr uint32_t MEDIA_CHANGE_SERR=(1u<<26);
static constexpr uint32_t ATA_SIGNATURE=0x00000101u;
enum class Result : uint8_t {
    Ok, Busy, InvalidDma, DeviceBusy, Timeout, DeviceError, CompletionUnconfirmed, ShortTransfer, MediaChanged
};
static constexpr uint32_t D2H_REGISTER_IRQ=1u, TASK_ACTIVE=0x88u, TASK_ERROR=0x21u;
// AHCI 1.3.1 §3.3.12: unrecovered and protocol errors. Recovered errors
// and PhyRdy changes remain independent of the command outcome.
static constexpr uint32_t SERR_COMMAND_ERROR=0xf00u;
static constexpr uint64_t TASK_SETTLE_NS=100000;
static constexpr unsigned TASK_SETTLE_POLLS=4096;

inline bool RegisterFis(uint32_t fis) { return (fis&0xffu)==0x34u; }
inline bool CleanRegisterFis(uint32_t fis) {
    const uint32_t status=(fis>>16)&0xffu;
    return RegisterFis(fis) && (status&(0x40u|TASK_ACTIVE|TASK_ERROR))==0x40u;
}
inline bool StaleBusyShadow(uint32_t task, uint32_t interruptStatus, uint32_t fis) {
    return task==0xc0u && (interruptStatus&D2H_REGISTER_IRQ) &&
        CleanRegisterFis(fis) && !(fis>>24);
}
// AHCI 1.3.1 §3.3.5: DHRS, PSS, DSS, SDBS and DPS report ordinary command
// progress. Every other PxIS bit is an error, link or hotplug event.
static constexpr uint32_t BENIGN_COMMAND_IRQ=0x2fu;
// Workstation may retire a command (CI clear, completion accepted) and leave
// PxTFD at exactly BSY|DRDY with no received FIS at all. Accept that shadow
// before the next issue only after this port's previous command completed
// successfully and the port shows no error, link or exchange evidence.
inline bool EmptyFisBusyShadow(uint32_t task, uint32_t interruptStatus, uint32_t serr,
                               uint32_t fis, bool previousCompletedOk) {
    return previousCompletedOk && task==0xc0u && !fis && !serr &&
        !(interruptStatus&~BENIGN_COMMAND_IRQ);
}
inline bool NeedsQuarantine(Result result) {
    // Never reuse a slot that might still DMA, and never proceed after an
    // explicit controller, device or media error. A retired but unconfirmed
    // transfer returns I/O failure without taking the port offline.
    return result==Result::Busy || result==Result::Timeout ||
        result==Result::DeviceError || result==Result::MediaChanged;
}

static constexpr uint64_t MEDIA_SETTLE_NS=10000000;
static constexpr unsigned MEDIA_SETTLE_POLLS=16384;

template<class Io> bool MediaChanged(Io& io, uint32_t& interruptStatus) {
    auto exchange=[&] {
        return (interruptStatus&MEDIA_CHANGE_IRQ) || (io.read(SERR)&MEDIA_CHANGE_SERR);
    };
    if(exchange())return true;
    uint32_t link=io.read(SSTS);
    if(link==0xffffffffu)return true;
    if((link&15u)==3 && (link&0xf00u)!=0x800u)return io.read(SIG)!=ATA_SIGNATURE;

    // An in-progress transition can show DET != 3. Confirm a DevSleep
    // sample once as well, and accept it if DET still proves an online
    // link. Exchange or presence evidence overrides the settling window.
    const uint64_t start=io.nowNs();
    for(unsigned poll=0;poll<MEDIA_SETTLE_POLLS;++poll) {
        const uint64_t now=io.nowNs();
        if(now<start || now-start>=MEDIA_SETTLE_NS)return true;
        io.pause();
        interruptStatus=io.read(IS);
        if(exchange())return true;
        link=io.read(SSTS);
        if(link==0xffffffffu)return true;
        if((link&15u)==3)return io.read(SIG)!=ATA_SIGNATURE;
    }
    // The poll cap also handles early boot and panic with a stalled clock.
    return true;
}

template<class Io> bool CommandError(Io& io, uint32_t interruptStatus, uint32_t task) {
    return (interruptStatus&ERROR_MASK) || task==0xffffffffu || (task&TASK_ERROR) ||
        (io.read(SERR)&SERR_COMMAND_ERROR);
}

// A single owner prepares slot zero. AHCI 1.3.1 §5.5.1 permits posting a
// command to an empty slot, and the HBA controls when it transmits. On the
// VMware 15ad:07e0 controller the task-file busy shadow can lag a verified
// prior command. Let the HBA issue the next command in that narrow case.
// The prior FIS can justify queueing only. Clear it before any new issue.
template<class Io> Result ArmCommand(Io& io) {
    io.postedWithStaleTaskBusy=false;
    uint32_t pending=io.read(IS);
    if(MediaChanged(io,pending))return Result::MediaChanged;
    uint32_t task=io.read(TFD);
    if(CommandError(io,pending,task))return Result::DeviceError;
    bool postWithBusyShadow=false;
    if(task&TASK_ACTIVE) {
        const uint64_t start=io.nowNs();
        for(unsigned poll=0;poll<TASK_SETTLE_POLLS && (task&TASK_ACTIVE);++poll) {
            const uint64_t now=io.nowNs();
            if(now<start || now-start>=TASK_SETTLE_NS)break;
            io.pause();
            pending=io.read(IS);
            if(MediaChanged(io,pending))return Result::MediaChanged;
            task=io.read(TFD);
            if(CommandError(io,pending,task))return Result::DeviceError;
        }
        if(io.allowStaleTaskBusy) {
            const uint32_t fis=io.receivedRegisterFis();
            // A DMA completion accepted before the received FIS appeared
            // may supply its late, clean FIS during this pre-issue wait.
            postWithBusyShadow=StaleBusyShadow(task,pending,fis) &&
                (fis==io.lastCompletionFis || io.lastDmaCompletedWithStaleTaskBusy);
            // The previous completion's FIS may never arrive (or was cleared
            // before that command's issue). Its accepted completion, clean
            // PxIS/PxSERR and the exact shadow still prove an idle device.
            postWithBusyShadow=postWithBusyShadow ||
                EmptyFisBusyShadow(task,pending,io.read(SERR),fis,io.lastCommandCompletedOk);
        }
        if((task&TASK_ACTIVE) && !postWithBusyShadow)return Result::DeviceBusy;
    }
    if(io.read(CI) || io.read(SACT))return Result::Busy;
    io.lastCompletionFis=0;
    io.lastDmaCompletedWithStaleTaskBusy=false;
    io.lastCommandCompletedOk=false;
    io.clearReceivedRegisterFis();
    // Preserve error and hotplug latches even if one asserts during W1C.
    io.write(IS,~(MEDIA_CHANGE_IRQ|ERROR_MASK));io.fence();
    pending=io.read(IS);
    if(MediaChanged(io,pending))return Result::MediaChanged;
    task=io.read(TFD);
    if(CommandError(io,pending,task))return Result::DeviceError;
    if((pending&D2H_REGISTER_IRQ) || ((task&TASK_ACTIVE) && !(postWithBusyShadow && task==0xc0u)))
        return Result::DeviceBusy;
    io.postedWithStaleTaskBusy=(task&TASK_ACTIVE)!=0;
    io.write(CI,1);
    return Result::Ok;
}

inline bool DmaRange(uint64_t address, uint32_t bytes, uint32_t alignment, bool dma64) {
    return address && !(address & (alignment-1)) && bytes && address<=~uint64_t{0}-(bytes-1) &&
           (dma64 || address+bytes-1<=0xffffffffull);
}
inline bool ControllerModeReady(uint32_t ghc) {
    return ghc!=0xffffffffu && (ghc&0x80000003u)==0x80000000u;
}

// Must be called with controller ownership and bus mastering disabled. The
// function refuses active firmware engines instead of taking over live DMA.
// Io must provide read(offset), write(offset,value), and fence().
template<class Io> Result Prepare(Io& io, uint64_t clb, uint64_t fis, bool dma64) {
    if(!DmaRange(clb,1024,1024,dma64) || !DmaRange(fis,256,256,dma64))return Result::InvalidDma;
    if((io.read(CMD)&(ST|FRE|CR|FR)) || io.read(CI) || io.read(SACT))return Result::Busy;
    io.write(IE,0);io.write(IS,0xffffffffu);io.write(SERR,0xffffffffu);
    io.write(CLB,uint32_t(clb));io.write(CLBU,uint32_t(clb>>32));
    io.write(FB,uint32_t(fis));io.write(FBU,uint32_t(fis>>32));io.fence();
    if(io.read(IE)!=0 || io.read(CLB)!=uint32_t(clb) || io.read(CLBU)!=uint32_t(clb>>32) ||
       io.read(FB)!=uint32_t(fis) || io.read(FBU)!=uint32_t(fis>>32))return Result::DeviceError;
    if((io.read(CMD)&(ST|FRE|CR|FR)) || io.read(CI) || io.read(SACT))return Result::Busy;
    return Result::Ok;
}

// Caller must have enabled bus mastering only after publishing owned DMA.
template<class Io> Result Start(Io& io) {
    uint32_t cmd=io.read(CMD);
    if(cmd&(ST|FRE|CR|FR))return Result::Busy;
    io.write(CMD,cmd|FRE);cmd=io.read(CMD);
    if(cmd==0xffffffffu || !(cmd&FRE))return Result::DeviceError;
    io.write(CMD,cmd|ST);
    cmd=io.read(CMD);
    return cmd!=0xffffffffu && (cmd&ST)?Result::Ok:Result::DeviceError;
}

// A false return means DMA quiescence was not proven: allocations remain
// pinned and the device must be quarantined until reboot.
template<class Io> bool Quiesce(Io& io) {
    uint32_t cmd=io.read(CMD);if(cmd==0xffffffffu)return false;
    io.write(CMD,cmd&~ST);
    if(!io.waitClear(CMD,CR))return false;
    cmd=io.read(CMD);if(cmd==0xffffffffu || (cmd&(ST|CR)))return false;
    io.write(CMD,cmd&~FRE);
    if(!io.waitClear(CMD,FR))return false;
    cmd=io.read(CMD);
    return cmd!=0xffffffffu && !(cmd&(ST|FRE|CR|FR));
}

// Called after the issue bit cleared. Neither a previous FIS nor a
// reset PRDBC can prove a new write succeeded. AHCI 1.3.1 §§5.3.8.1–2
// require the HBA to publish status and PRDBC before clearing the slot.
template<class Io> Result CompleteCommand(Io& io, uint32_t expectedBytes) {
    uint32_t completed,task,fis;
    const auto sample=[&] {
        io.fence();
        completed=io.read(IS);
        if(MediaChanged(io,completed))return Result::MediaChanged;
        if(io.read(CI) || io.read(SACT))return Result::Busy;
        task=io.read(TFD);
        fis=io.receivedRegisterFis();
        if(CommandError(io,completed,task) || (RegisterFis(fis) && ((fis>>16)&TASK_ERROR)))
            return Result::DeviceError;
        return Result::Ok;
    };
    const auto activeFis=[&] { return RegisterFis(fis) && ((fis>>16)&TASK_ACTIVE); };
    const auto activeTask=[&] {
        return (task&TASK_ACTIVE) && !(io.allowStaleTaskBusy && StaleBusyShadow(task,completed,fis));
    };
    auto result=sample();
    if(result!=Result::Ok)return result;
    const bool vmwareDma=io.allowStaleTaskBusy && expectedBytes;
    if(vmwareDma && (activeTask() || activeFis())) {
        // Workstation can clear CI before publishing the received FIS
        // and dropping the TFD busy shadow. Recheck for late faults too.
        const uint64_t start=io.nowNs();
        for(unsigned poll=0;poll<TASK_SETTLE_POLLS && (activeTask() || activeFis());++poll) {
            const uint64_t now=io.nowNs();
            if(now<start || now-start>=TASK_SETTLE_NS)break;
            io.pause();
            result=sample();
            if(result!=Result::Ok)return result;
        }
    }
    if(activeFis())return Result::CompletionUnconfirmed;
    const bool cleanFis=CleanRegisterFis(fis);
    if(!expectedBytes) {
        const uint32_t engine=io.read(CMD);
        if(engine==0xffffffffu || (engine&(ST|FRE))!=(ST|FRE))return Result::DeviceError;
        // CI alone is insufficient for no-data commands if the issue write was lost.
        if(!(completed&D2H_REGISTER_IRQ) || !cleanFis)return Result::CompletionUnconfirmed;
    }
    // ArmCommand cleared the FIS and acknowledged old DHRS before issuing.
    // On this virtual HBA only, fresh DHRS and the full new byte count
    // confirm retired DMA even if the receive buffer still reads empty
    // and PxTFD is exactly the observed BSY|DRDY shadow.
    const bool delayedFis=vmwareDma && task==0xc0u && !fis && (completed&D2H_REGISTER_IRQ);
    if(activeTask() && !delayedFis)return Result::CompletionUnconfirmed;
    if(io.transferred()!=expectedBytes)return Result::ShortTransfer;
    if(cleanFis)io.lastCompletionFis=fis;
    io.lastDmaCompletedWithStaleTaskBusy=delayedFis;
    // The only successful return for both DMA and no-data commands.
    io.lastCommandCompletedOk=true;
    return Result::Ok;
}

// The caller prepared slot zero and holds its sleepable serialization lock.
// Io::transferred reads PRDBC after the completion fence.
template<class Io> Result Submit(Io& io, uint32_t expectedBytes) {
    if(!expectedBytes || io.read(CI) || io.read(SACT))return Result::Busy;
    const auto armed=ArmCommand(io);
    if(armed!=Result::Ok)return armed;
    if(!io.waitClear(CI,1))return Result::Timeout;
    return CompleteCommand(io,expectedBytes);
}

// A flush may take much longer than ordinary sector I/O, so its caller
// provides a separate bounded completion wait.
template<class Io> Result SubmitNoData(Io& io) {
    const uint32_t cmd=io.read(CMD),ci=io.read(CI),sact=io.read(SACT);
    if(cmd==0xffffffffu || ci==0xffffffffu || sact==0xffffffffu ||
       (cmd&(ST|FRE))!=(ST|FRE))return Result::DeviceError;
    if(ci || sact)return Result::Busy;
    const auto armed=ArmCommand(io);
    if(armed!=Result::Ok)return armed;
    if(!io.waitClearFlush(CI,1))return Result::Timeout;
    return CompleteCommand(io,0);
}
} // namespace ahci_ro
} // namespace brook
