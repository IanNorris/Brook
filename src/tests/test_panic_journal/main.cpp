#include "../../kernel/src/panic_journal.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
using namespace brook;

struct MemoryDisk {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(PanicJournalSectors*PanicJournalSector);
    unsigned calls=0,failAt=0,writes=0,flushes=0;
    bool partial=false;
    std::vector<unsigned> written;
    bool fail() { return ++calls==failAt; }
    bool read(uint32_t sector,uint8_t* out) {
        assert(sector<PanicJournalSectors);if(fail())return false;
        std::memcpy(out,bytes.data()+sector*PanicJournalSector,PanicJournalSector);return true;
    }
    bool write(uint32_t sector,const uint8_t* in) {
        assert(sector<PanicJournalSectors);++writes;written.push_back(sector);
        if(fail()) {if(partial)std::memcpy(bytes.data()+sector*PanicJournalSector,in,173);return false;}
        std::memcpy(bytes.data()+sector*PanicJournalSector,in,PanicJournalSector);return true;
    }
    bool flush() {++flushes;return !fail();}
    void reset() {calls=failAt=writes=flushes=0;partial=false;written.clear();}
};
int main() {
    const uint8_t standard[]={'1','2','3','4','5','6','7','8','9'};
    assert(PanicJournalCrc(standard,sizeof(standard))==0xcbf43926u);
    MemoryDisk seed;
    std::vector<uint8_t> first(5031),second(PanicJournalPayloadMax),out(PanicJournalPayloadMax);
    for(size_t i=0;i<first.size();++i)first[i]=uint8_t(i*17+9);
    for(size_t i=0;i<second.size();++i)second[i]=uint8_t(i*29+31);
    assert(PanicJournalWrite(seed,first.data(),first.size())==PanicJournalResult::Ok);
    PanicJournalRecord record;
    assert(PanicJournalReadLatest(seed,out.data(),out.size(),record)==PanicJournalResult::Ok);
    assert(record.sequence==1 && record.bytes==first.size());
    assert(std::equal(first.begin(),first.end(),out.begin()));
    assert(PanicJournalReadRanked(seed,out.data(),out.size(),record,1)==PanicJournalResult::NoValidRecord);
    assert(PanicJournalReadRanked(seed,out.data(),out.size(),record,2)==PanicJournalResult::Invalid);
    MemoryDisk success=seed;success.reset();
    assert(PanicJournalWrite(success,second.data(),second.size())==PanicJournalResult::Ok);
    const unsigned operations=success.calls;
    assert(success.flushes==2 && success.writes==129);
    assert(std::all_of(success.written.begin(),success.written.end(),[](unsigned s){return s>=PanicJournalSlotSectors;}));
    assert(PanicJournalReadLatest(success,out.data(),out.size(),record)==PanicJournalResult::Ok);
    assert(record.sequence==2 && record.bytes==second.size());
    assert(std::equal(second.begin(),second.end(),out.begin()));
    assert(PanicJournalReadRanked(success,out.data(),out.size(),record,1)==PanicJournalResult::Ok);
    assert(record.sequence==1 && std::equal(first.begin(),first.end(),out.begin()));
    // Every individual read, sector write, and flush is independently failed;
    // a torn sector write must never destroy the previous committed slot.
    for(unsigned failure=1;failure<=operations;++failure)for(bool partial:{false,true}) {
        MemoryDisk fault=seed;fault.reset();fault.failAt=failure;fault.partial=partial;
        assert(PanicJournalWrite(fault,second.data(),second.size())!=PanicJournalResult::Ok);
        fault.reset();PanicJournalRecord old;
        assert(PanicJournalInspect(fault,0,old)==PanicJournalResult::Ok && old.valid && old.sequence==1);
        assert(std::equal(fault.bytes.begin(),fault.bytes.begin()+PanicJournalSlotSectors*PanicJournalSector,
                          seed.bytes.begin()));
    }
    // Corrupt the latest data: reader must fall back to the older valid slot.
    success.bytes[PanicJournalSlotSectors*PanicJournalSector+PanicJournalSector+5]^=1;
    assert(PanicJournalReadLatest(success,out.data(),out.size(),record)==PanicJournalResult::Ok);
    assert(record.sequence==1 && std::equal(first.begin(),first.end(),out.begin()));
    assert(PanicJournalReadRanked(success,out.data(),out.size(),record,1)==PanicJournalResult::NoValidRecord);
    MemoryDisk corrupt;corrupt.bytes[0]=1;
    assert(PanicJournalWrite(corrupt,first.data(),first.size())==PanicJournalResult::NoValidRecord);
    assert(corrupt.writes==0);
    // A recognizable torn first header must not prevent the next panic from
    // using the other slot, and that damaged evidence must remain untouched.
    for(unsigned damagedSlot=0;damagedSlot<2;++damagedSlot) {
        MemoryDisk torn;
        auto* header=torn.bytes.data()+damagedSlot*PanicJournalSlotSectors*PanicJournalSector;
        std::memcpy(header,"BRKPAN01",8);PanicJournalPut32(header+8,1);
        PanicJournalPut32(header+12,123); // intentionally invalid header CRC
        const auto original=torn.bytes;
        PanicJournalRecord a{},b{};
        assert(PanicJournalInspect(torn,0,a)==PanicJournalResult::Ok);
        assert(PanicJournalInspect(torn,1,b)==PanicJournalResult::Ok);
        assert(PanicJournalWritable(a,b));
        assert(PanicJournalWrite(torn,first.data(),first.size())==PanicJournalResult::Ok);
        const auto begin=damagedSlot*PanicJournalSlotSectors*PanicJournalSector;
        assert(std::equal(original.begin()+begin,original.begin()+begin+PanicJournalSlotSectors*PanicJournalSector,torn.bytes.begin()+begin));
        assert(PanicJournalReadLatest(torn,out.data(),out.size(),record)==PanicJournalResult::Ok);
        assert(record.sequence==1 && std::equal(first.begin(),first.end(),out.begin()));
    }
    assert(PanicJournalWrite(seed,nullptr,1)==PanicJournalResult::Invalid);
    assert(PanicJournalWrite(seed,first.data(),0)==PanicJournalResult::Invalid);
    assert(PanicJournalWrite(seed,first.data(),PanicJournalPayloadMax+1)==PanicJournalResult::Invalid);
}
