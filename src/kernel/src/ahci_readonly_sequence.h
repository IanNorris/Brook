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
enum class Result : uint8_t { Ok, Busy, InvalidDma, DeviceBusy, Timeout, DeviceError, ShortTransfer, MediaChanged };

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

// Preserve hotplug latches even if they assert between the check and the
// acknowledgement. SERR is cleared only at Prepare, before identification.
template<class Io> Result ArmCommand(Io& io) {
    uint32_t pending=io.read(IS);
    if(MediaChanged(io,pending))return Result::MediaChanged;
    const uint32_t task=io.read(TFD);
    if(task==0xffffffffu)return Result::DeviceError;
    if(task&0x88u)return Result::DeviceBusy;
    io.write(IS,~MEDIA_CHANGE_IRQ);io.fence();
    pending=io.read(IS);
    if(MediaChanged(io,pending))return Result::MediaChanged;
    if(pending&1u)return Result::DeviceError;
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

// The caller prepared slot zero and holds its sleepable serialization lock.
// Io::transferred reads PRDBC after the completion fence.
template<class Io> Result Submit(Io& io, uint32_t expectedBytes) {
    if(!expectedBytes || io.read(CI) || io.read(SACT))return Result::Busy;
    const auto armed=ArmCommand(io);
    if(armed!=Result::Ok)return armed;
    if(!io.waitClear(CI,1))return Result::Timeout;
    io.fence();
    uint32_t completed=io.read(IS);
    if(MediaChanged(io,completed))return Result::MediaChanged;
    if((completed&ERROR_MASK) || (io.read(TFD)&1u))return Result::DeviceError;
    if(io.transferred()!=expectedBytes)return Result::ShortTransfer;
    return Result::Ok;
}

// For no-data commands only. A flush may take much longer than ordinary
// sector I/O, so its caller provides a separate bounded completion wait.
// This helper does not construct or select any ATA command itself.
template<class Io> Result SubmitNoData(Io& io) {
    const uint32_t cmd=io.read(CMD),ci=io.read(CI),sact=io.read(SACT);
    if(cmd==0xffffffffu || ci==0xffffffffu || sact==0xffffffffu ||
       (cmd&(ST|FRE))!=(ST|FRE))return Result::DeviceError;
    if(ci || sact)return Result::Busy;
    const auto armed=ArmCommand(io);
    if(armed!=Result::Ok)return armed;
    if(!io.waitClearFlush(CI,1))return Result::Timeout;
    io.fence();
    uint32_t completed=io.read(IS);
    if(MediaChanged(io,completed))return Result::MediaChanged;
    const uint32_t task=io.read(TFD),engine=io.read(CMD);
    // A newly received device-to-host register FIS proves that a no-data
    // command completed; CI alone may already be clear if a write was lost.
    if(task==0xffffffffu || engine==0xffffffffu ||
       !(completed&1u) || (completed&ERROR_MASK) || (task&0xa9u) ||
       (engine&(ST|FRE))!=(ST|FRE))return Result::DeviceError;
    return io.transferred()==0 ? Result::Ok : Result::ShortTransfer;
}
} // namespace ahci_ro
} // namespace brook
