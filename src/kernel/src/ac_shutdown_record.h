#pragma once
#include "update/payload.h"

namespace brook {
constexpr uint32_t AcShutdownSectorBytes=512;
constexpr unsigned AcShutdownSlots=2;
struct AcShutdownRecord {
    uint64_t sequence=0;
    char bootId[33]{};
};

inline bool AcShutdownBootId(const char* id) {
    bool nonzero=false;
    for(unsigned i=0;i<32;++i) {
        const char c=id[i];
        if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
        nonzero|=c!='0';
    }
    return nonzero && !id[32];
}

// The magic identifies version 1 and a checked AC-loss power-off. These two
// sectors are independent of the panic journal and the boot guard records.
inline constexpr uint8_t AcShutdownMagic[]={'B','R','K','P','W','R','0','1'};
constexpr unsigned AcShutdownCrcOffset=AcShutdownSectorBytes-4;
enum class AcShutdownSlotKind { Blank, Foreign, Torn, Valid };

inline AcShutdownSlotKind AcShutdownDecode(const uint8_t (&sector)[AcShutdownSectorBytes],AcShutdownRecord& out) {
    out={};
    bool blank=true;
    for(const auto byte:sector)blank&=byte==0;
    if(blank)return AcShutdownSlotKind::Blank;
    if(!BrookUpdateEqual(sector,AcShutdownMagic,sizeof(AcShutdownMagic)))return AcShutdownSlotKind::Foreign;
    if(BrookUpdateGet32(sector+AcShutdownCrcOffset)!=BrookUpdateCrc(sector,AcShutdownCrcOffset))
        return AcShutdownSlotKind::Torn;
    AcShutdownRecord record{};
    record.sequence=BrookUpdateGet64(sector+8);
    for(unsigned i=0;i<32;++i)record.bootId[i]=char(sector[16+i]);
    if(!record.sequence || !AcShutdownBootId(record.bootId))return AcShutdownSlotKind::Foreign;
    for(unsigned i=48;i<AcShutdownCrcOffset;++i)if(sector[i])return AcShutdownSlotKind::Foreign;
    out=record;
    return AcShutdownSlotKind::Valid;
}

struct AcShutdownSelection {
    AcShutdownSlotKind kind[AcShutdownSlots]{};
    AcShutdownRecord latest{};
    int newest=-1;
};

template<class Io>
bool AcShutdownRead(Io& io,AcShutdownSelection& out) {
    out={};
    uint8_t sector[AcShutdownSectorBytes];
    for(unsigned i=0;i<AcShutdownSlots;++i) {
        if(!io.read(i,sector))return false;
        AcShutdownRecord record{};
        out.kind[i]=AcShutdownDecode(sector,record);
        if(out.kind[i]!=AcShutdownSlotKind::Valid)continue;
        if(record.sequence==out.latest.sequence)return false;
        if(record.sequence>out.latest.sequence) {
            out.latest=record;
            out.newest=int(i);
        }
    }
    return true;
}

template<class Io>
bool AcShutdownWrite(Io& io,const char (&bootId)[33]) {
    if(!AcShutdownBootId(bootId))return false;
    AcShutdownSelection slots{};
    if(!AcShutdownRead(io,slots) || slots.latest.sequence==~uint64_t{0})return false;
    if(slots.kind[0]==AcShutdownSlotKind::Foreign || slots.kind[1]==AcShutdownSlotKind::Foreign)return false;
    const unsigned target=slots.newest>=0?unsigned(1-slots.newest):
        slots.kind[0]==AcShutdownSlotKind::Blank?0:1;
    const uint64_t sequence=slots.latest.sequence+1;
    uint8_t sector[AcShutdownSectorBytes]{};
    for(unsigned i=0;i<sizeof(AcShutdownMagic);++i)sector[i]=AcShutdownMagic[i];
    BrookUpdatePut64(sector+8,sequence);
    for(unsigned i=0;i<32;++i)sector[16+i]=uint8_t(bootId[i]);
    BrookUpdatePut32(sector+AcShutdownCrcOffset,BrookUpdateCrc(sector,AcShutdownCrcOffset));
    if(!io.write(target,sector) || !io.flush())return false;
    uint8_t verified[AcShutdownSectorBytes];
    return io.read(target,verified) && BrookUpdateEqual(sector,verified,sizeof(sector));
}
}
