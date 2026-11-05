#pragma once
#include <stdint.h>

namespace brook {
// Two fixed slots, each a header sector followed by at most 64 KiB of data.
// All addresses below are relative to an already admitted, preallocated area.
// The caller must enforce exclusive ownership, device identity and bounds;
// writes and flush/readback here never allocate or modify filesystem metadata.
constexpr uint32_t PanicJournalSector = 512;
constexpr uint32_t PanicJournalPayloadMax = 65536;
constexpr uint32_t PanicJournalSlotSectors = 1 + PanicJournalPayloadMax / PanicJournalSector;
constexpr uint32_t PanicJournalSectors = 2 * PanicJournalSlotSectors;

enum class PanicJournalResult { Ok, Invalid, ReadFailed, NoValidRecord, Ambiguous,
                                Exhausted, WriteFailed, FlushFailed, VerifyFailed };
struct PanicJournalRecord { bool valid=false, blank=false; uint64_t sequence=0; uint32_t bytes=0, checksum=0; };

inline uint32_t PanicJournalCrcUpdate(uint32_t crc, const uint8_t* data, uint32_t size) {
    while (size--) { crc ^= *data++; for (unsigned i=0;i<8;++i) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1u))); }
    return crc;
}
inline void PanicJournalPut32(uint8_t* p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
inline void PanicJournalPut64(uint8_t* p,uint64_t v) {for(unsigned i=0;i<8;++i)p[i]=uint8_t(v>>(8*i));}
inline uint32_t PanicJournalGet32(const uint8_t* p) {uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);return v;}
inline uint64_t PanicJournalGet64(const uint8_t* p) {uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[i])<<(8*i);return v;}
inline uint32_t PanicJournalCrc(const uint8_t* p,uint32_t n) {return ~PanicJournalCrcUpdate(~uint32_t(0),p,n);}

// Io supplies read/write of exactly one 512-byte sector and a checked flush.
// A successful read must bypass any stale file or filesystem cache. False
// means an uncertain I/O outcome; callers must not blindly retry a write.
template<class Io>
PanicJournalResult PanicJournalInspect(Io& io,unsigned slot,PanicJournalRecord& out) {
    out={}; if(slot>1)return PanicJournalResult::Invalid;
    uint8_t sector[PanicJournalSector]{};const uint32_t base=slot*PanicJournalSlotSectors;
    if(!io.read(base,sector))return PanicJournalResult::ReadFailed;
    bool blank=true;for(auto b:sector)if(b){blank=false;break;}out.blank=blank;
    const uint8_t magic[8]={'B','R','K','P','A','N','0','1'};
    for(unsigned i=0;i<8;++i)if(sector[i]!=magic[i])return PanicJournalResult::Ok;
    const uint32_t headerCrc=PanicJournalGet32(sector+28);PanicJournalPut32(sector+28,0);
    if(PanicJournalGet32(sector+8)!=1 || PanicJournalCrc(sector,sizeof(sector))!=headerCrc)return PanicJournalResult::Ok;
    const uint32_t bytes=PanicJournalGet32(sector+12);const uint64_t sequence=PanicJournalGet64(sector+16);
    const uint32_t checksum=PanicJournalGet32(sector+24);
    if(!bytes || bytes>PanicJournalPayloadMax || !sequence)return PanicJournalResult::Ok;
    uint32_t remaining=bytes,crc=~uint32_t(0),index=1;
    while(remaining) {
        if(!io.read(base+index++,sector))return PanicJournalResult::ReadFailed;
        const uint32_t n=remaining<PanicJournalSector?remaining:PanicJournalSector;
        crc=PanicJournalCrcUpdate(crc,sector,n);remaining-=n;
    }
    if(~crc!=checksum)return PanicJournalResult::Ok;
    out.valid=true;out.sequence=sequence;out.bytes=bytes;out.checksum=checksum;
    return PanicJournalResult::Ok;
}

// The output may be partially filled if an I/O error occurs; callers may use
// it only after Ok. Recheck the copied payload so media changes during the
// two reads cannot silently produce a successful inconsistent capture.
template<class Io>
PanicJournalResult PanicJournalReadLatest(Io& io,uint8_t* output,uint32_t capacity,PanicJournalRecord& result) {
    result={};if(!output)return PanicJournalResult::Invalid;
    PanicJournalRecord slots[2];
    for(unsigned i=0;i<2;++i) {const auto r=PanicJournalInspect(io,i,slots[i]);if(r!=PanicJournalResult::Ok)return r;}
    if(slots[0].valid && slots[1].valid && slots[0].sequence==slots[1].sequence)return PanicJournalResult::Ambiguous;
    int newest=-1;if(slots[0].valid)newest=0;
    if(slots[1].valid && (newest<0 || slots[1].sequence>slots[0].sequence))newest=1;
    if(newest<0)return PanicJournalResult::NoValidRecord;
    if(capacity<slots[newest].bytes)return PanicJournalResult::Invalid;
    uint8_t sector[PanicJournalSector];uint32_t done=0,index=1;
    const uint32_t base=uint32_t(newest)*PanicJournalSlotSectors;
    while(done<slots[newest].bytes) {
        if(!io.read(base+index++,sector))return PanicJournalResult::ReadFailed;
        const uint32_t n=slots[newest].bytes-done<PanicJournalSector?slots[newest].bytes-done:PanicJournalSector;
        for(uint32_t j=0;j<n;++j)output[done+j]=sector[j];
        done+=n;
    }
    if(PanicJournalCrc(output,done)!=slots[newest].checksum)return PanicJournalResult::VerifyFailed;
    result=slots[newest];return PanicJournalResult::Ok;
}

template<class Io>
PanicJournalResult PanicJournalWrite(Io& io,const uint8_t* payload,uint32_t bytes) {
    if(!payload || !bytes || bytes>PanicJournalPayloadMax)return PanicJournalResult::Invalid;
    PanicJournalRecord slots[2];
    for(unsigned i=0;i<2;++i) {const auto r=PanicJournalInspect(io,i,slots[i]);if(r!=PanicJournalResult::Ok)return r;}
    if(slots[0].valid && slots[1].valid && slots[0].sequence==slots[1].sequence)return PanicJournalResult::Ambiguous;
    int newest=-1;
    if(slots[0].valid)newest=0;
    if(slots[1].valid && (newest<0 || slots[1].sequence>slots[0].sequence))newest=1;
    if(newest<0 && (!slots[0].blank || !slots[1].blank))return PanicJournalResult::NoValidRecord;
    if(newest>=0 && slots[newest].sequence==~uint64_t(0))return PanicJournalResult::Exhausted;
    const unsigned target=newest<0?0:unsigned(1-newest);
    const uint64_t sequence=newest<0?1:slots[newest].sequence+1;
    const uint32_t checksum=PanicJournalCrc(payload,bytes),base=target*PanicJournalSlotSectors;
    uint8_t sector[PanicJournalSector]{};
    uint32_t done=0,index=1;
    while(done<bytes) {
        for(auto& b:sector)b=0;
        const uint32_t n=bytes-done<PanicJournalSector?bytes-done:PanicJournalSector;
        for(uint32_t j=0;j<n;++j)sector[j]=payload[done+j];
        if(!io.write(base+index++,sector))return PanicJournalResult::WriteFailed;
        done+=n;
    }
    if(!io.flush())return PanicJournalResult::FlushFailed;
    // Verify the durable data independently before publishing its header.
    uint32_t remaining=bytes,crc=~uint32_t(0);index=1;
    while(remaining) {
        if(!io.read(base+index++,sector))return PanicJournalResult::ReadFailed;
        const uint32_t n=remaining<PanicJournalSector?remaining:PanicJournalSector;
        crc=PanicJournalCrcUpdate(crc,sector,n);remaining-=n;
    }
    if(~crc!=checksum)return PanicJournalResult::VerifyFailed;
    for(auto& b:sector)b=0;
    const uint8_t magic[8]={'B','R','K','P','A','N','0','1'};
    for(unsigned i=0;i<8;++i)sector[i]=magic[i];
    PanicJournalPut32(sector+8,1);PanicJournalPut32(sector+12,bytes);
    PanicJournalPut64(sector+16,sequence);PanicJournalPut32(sector+24,checksum);
    PanicJournalPut32(sector+28,PanicJournalCrc(sector,sizeof(sector)));
    if(!io.write(base,sector))return PanicJournalResult::WriteFailed;
    if(!io.flush())return PanicJournalResult::FlushFailed;
    PanicJournalRecord written;const auto r=PanicJournalInspect(io,target,written);
    if(r!=PanicJournalResult::Ok)return r;
    if(!written.valid || written.sequence!=sequence || written.bytes!=bytes || written.checksum!=checksum)
        return PanicJournalResult::VerifyFailed;
    return PanicJournalResult::Ok;
}
}
