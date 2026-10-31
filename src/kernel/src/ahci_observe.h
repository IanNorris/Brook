#pragma once
#include <stdint.h>

namespace brook {
struct BootProtocol;
struct AhciObservedPort { uint32_t index, status, signature, command, taskFile; };
struct AhciObservation {
    bool requested, valid;
    const char* reason = "not requested";
    uint32_t controllerCount, vendor, device, bus, slot, function;
    uint32_t capability, implemented, version, portCount;
    uint64_t bar;
    AhciObservedPort ports[32];
};

// Physical register snapshot only: no AHCI commands, DMA, disk reads or writes.
void AhciObserveConfigure(const BootProtocol* boot);
const AhciObservation& AhciObserveRead();
// Valid only for a successful observation; borrowed for this boot's lifetime.
volatile uint32_t* AhciObserveMappedPage();

inline bool AhciObserveBar(uint32_t raw, uint64_t& base) {
    if ((raw & 1u) || (raw & 6u)) return false; // BAR5 must be 32-bit MMIO
    base = uint64_t(raw & ~15u);
    return base && !(base & 4095u) && base <= 0xfffff000u;
}
inline bool AhciObservePortMap(uint32_t cap, uint32_t pi) {
    if (cap == 0xffffffffu || pi == 0xffffffffu) return false;
    const uint32_t ports = (cap & 31u) + 1;
    // This diagnostic maps only the first register page (ports 0..29).
    return ports <= 30 && !(pi >> ports);
}

// Io supplies PCI command read/write and AHCI register read only. Never enable
// bus mastering and never write a controller register or issue an ATA command.
template<class Io> void AhciObserveRegisters(Io& io, AhciObservation& out) {
    const uint16_t original=io.command();
    if (original==0xffffu) {out.reason="PCI command unavailable";return;}
    const bool changed=!(original&2u);
    if (changed && (original&4u)) {out.reason="firmware bus mastering active";return;}
    if (changed) io.command(uint16_t(original|2u));
    const uint16_t enabled=io.command();
    if (enabled==0xffffu || enabled!=uint16_t(original|2u)) {
        if(changed) {
            io.command(original);
            if(io.command()!=original) {out.reason="PCI command restore failed";return;}
        }
        out.reason="AHCI memory decoding unavailable";return;
    }
    const uint32_t cap=io.read(0),pi=io.read(0x0c),version=io.read(0x10);
    if (AhciObservePortMap(cap,pi)) {
        out.capability=cap;out.implemented=pi;out.version=version;
        const uint32_t count=(cap&31u)+1;
        for (uint32_t i=0;i<count;++i) if (pi&(1u<<i)) {
            auto& p=out.ports[out.portCount++];const uint32_t off=0x100+i*0x80;
            p.index=i;p.status=io.read(off+0x28);p.signature=io.read(off+0x24);
            p.command=io.read(off+0x18);p.taskFile=io.read(off+0x20);
        }
        out.valid=true;out.reason="read-only register snapshot";
    } else out.reason="invalid AHCI capability or port map";
    if (changed) {
        io.command(original);
        if (io.command()!=original) {out.valid=false;out.reason="PCI command restore failed";}
    }
}
} // namespace brook
