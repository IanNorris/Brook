#include "page_store.h"
#include "compressed_page_pool.h"

namespace brook {
uint64_t PageStoreChecksum(const void* bytes,uint64_t length) {
    // FNV-1a is an accidental-corruption check, not authentication.
    auto* p=static_cast<const uint8_t*>(bytes);
    uint64_t hash=14695981039346656037ULL;
    for(uint64_t i=0;i<length;++i)hash=(hash^p[i])*1099511628211ULL;
    return hash;
}
bool PageStoreHeaderValid(const PageStoreHeader& h,uint64_t deviceBytes) {
    bool identity=false;for(auto byte:h.identity)identity|=byte!=0;
    return h.magic==PAGE_STORE_MAGIC && h.version==1 && h.pageBytes==4096 &&
        h.dataOffset==PAGE_STORE_HEADER_BYTES && h.slots && h.slots<0xffffffffULL &&
        h.dataOffset<=deviceBytes && h.slots<=(deviceBytes-h.dataOffset)/4096 &&
        identity && h.checksum==PageStoreChecksum(&h,48);
}
bool PageStore::Initialize(const PageStoreHeader& header,uint64_t deviceBytes,
                          PageStoreSlot* records,uint64_t count,PageStoreIo io) {
    // No live-device replacement or resetting generations on an attached store.
    if(records_ || !records || !io.read || !io.write ||
       !PageStoreHeaderValid(header,deviceBytes) || count<header.slots)return false;
    records_=records;io_=io;dataOffset_=header.dataOffset;stats_.slots=header.slots;
    for(uint64_t i=0;i<header.slots;++i)records_[i]={0,0,0,PageSlotState::Free,0,0};
    return true;
}
bool PageStore::AttachCompression(PageStoreCompression c) {
    if(!records_ || started_ || compression_.pool || !c.pool || !c.pool->Capacity() ||
       !c.encode || !c.decode || !c.encodedScratch || !c.spillScratch || c.encodedScratch==c.spillScratch)return false;
    compression_=c;return true;
}
PageStoreSlot* PageStore::Find(uint64_t token) {
    const uint64_t index=(token&0xffffffffULL)-1;
    if(!records_ || index>=stats_.slots || !uint32_t(token>>32))return nullptr;
    auto& r=records_[index];
    return r.references && r.generation==uint32_t(token>>32)?&r:nullptr;
}
uint64_t PageStore::Offset(uint64_t token) const {
    return dataOffset_+((token&0xffffffffULL)-1)*4096;
}
uint64_t PageStore::Reserve() {
    if(!records_)return 0;
    started_=true;
    for(uint64_t n=0;n<stats_.slots;++n) {
        uint64_t i=(hint_+n)%stats_.slots;auto& r=records_[i];
        if(r.state!=PageSlotState::Free)continue;
        if(r.generation==0xffffffffu){r.state=PageSlotState::Exhausted;continue;}
        ++r.generation;r.references=1;r.checksum=0;r.ramOffset=0;r.compressedBytes=0;r.state=PageSlotState::Reserved;
        ++stats_.used;hint_=(i+1)%stats_.slots;
        return (uint64_t(r.generation)<<32)|(i+1);
    }
    return 0;
}
bool PageStore::Retain(uint64_t token) {
    auto* r=Find(token);
    if(!r || (r->state!=PageSlotState::Stored && r->state!=PageSlotState::StoredRam) || r->references==0xffffffffu)return false;
    ++r->references;return true;
}
bool PageStore::Release(uint64_t token) {
    auto* r=Find(token);if(!r)return false;
    if(r->references>1){--r->references;return true;}
    if(r->state==PageSlotState::StoredRam) {
        if(!compression_.pool->Release(r->ramOffset,r->compressedBytes))return false;
        --stats_.ramSlots;--stats_.stored;
    } else if(r->state==PageSlotState::Stored){--stats_.diskSlots;--stats_.stored;}
    --stats_.used;r->references=0;r->state=PageSlotState::Free;r->checksum=0;
    r->ramOffset=0;r->compressedBytes=0;return true;
}
int PageStore::Store(uint64_t token,const void* page) {
    auto* r=Find(token);if(!r || !page)return -22;
    if(r->state!=PageSlotState::Reserved || r->references!=1)return -16;
    const uint64_t checksum=PageStoreChecksum(page,4096);
    if(compression_.pool) {
        int bytes=compression_.encode(compression_.context,page,compression_.encodedScratch,4096);
        uint32_t offset=0;
        // Require at least 25% savings after 64-byte allocation rounding.
        if(bytes>0 && bytes<=3072 && compression_.pool->Put(compression_.encodedScratch,uint16_t(bytes),offset)) {
            r->checksum=checksum;r->ramOffset=offset;r->compressedBytes=bytes;r->state=PageSlotState::StoredRam;
            ++stats_.stored;++stats_.writes;++stats_.ramWrites;++stats_.ramSlots;return 0;
        }
        ++stats_.compressionFallbacks;
    }
    const int result=io_.write(io_.context,Offset(token),page,4096);
    if(result!=4096){++stats_.ioErrors;return result<0?result:-5;}
    r->checksum=checksum;r->state=PageSlotState::Stored;++stats_.stored;++stats_.writes;++stats_.diskWrites;++stats_.diskSlots;
    return 0;
}
int PageStore::Load(uint64_t token,void* page) {
    auto* r=Find(token);if(!r || !page)return -22;
    if(r->state==PageSlotState::StoredRam) {
        int result=DecodeRam(*r,page);if(result)return result;
        ++stats_.reads;++stats_.ramReads;return 0;
    }
    if(r->state!=PageSlotState::Stored)return -11;
    const int result=io_.read(io_.context,Offset(token),page,4096);
    if(result!=4096){++stats_.ioErrors;return result<0?result:-5;}
    if(PageStoreChecksum(page,4096)!=r->checksum){++stats_.corruptReads;return -5;}
    ++stats_.reads;++stats_.diskReads;return 0;
}
int PageStore::DecodeRam(const PageStoreSlot& r,void* page) {
    const auto* bytes=compression_.pool->Get(r.ramOffset,r.compressedBytes);
    if(!bytes || r.compressedBytes>3072 ||
       compression_.decode(compression_.context,bytes,r.compressedBytes,page,4096)!=4096) {
        ++stats_.decodeErrors;return -5;
    }
    if(PageStoreChecksum(page,4096)!=r.checksum){++stats_.corruptReads;return -5;}
    return 0;
}
int PageStore::Spill(uint64_t token) {
    auto* r=Find(token);if(!r)return -22;
    if(r->state==PageSlotState::Stored)return 0;
    if(r->state!=PageSlotState::StoredRam)return -11;
    int result=DecodeRam(*r,compression_.spillScratch);
    if(result){++stats_.spillFailures;return result;}
    result=io_.write(io_.context,Offset(token),compression_.spillScratch,4096);
    if(result!=4096){++stats_.ioErrors;++stats_.spillFailures;return result<0?result:-5;}
    // No fallible allocation/publication remains. Free only after full I/O;
    // partial writes to this slot's raw location never invalidate RAM bytes.
    if(!compression_.pool->Release(r->ramOffset,r->compressedBytes)){++stats_.spillFailures;return -5;}
    r->state=PageSlotState::Stored;r->ramOffset=0;r->compressedBytes=0;
    --stats_.ramSlots;++stats_.diskSlots;++stats_.diskWrites;++stats_.spillSuccesses;return 0;
}

}
