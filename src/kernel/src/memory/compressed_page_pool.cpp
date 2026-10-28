#include "compressed_page_pool.h"
namespace brook {
namespace {
uint64_t Mask(unsigned n){return n==64?~uint64_t{0}:(uint64_t{1}<<n)-1;}
}
bool CompressedPagePool::Initialize(void* bytes,uint32_t capacity,uint64_t* bitmap,uint32_t words) {
    if(bytes_ || !bytes || !bitmap || !capacity || capacity%Unit || capacity/Unit>uint64_t(words)*64)return false;
    bytes_=static_cast<uint8_t*>(bytes);bits_=bitmap;units_=capacity/Unit;
    for(uint32_t i=0;i<(units_+63)/64;++i)bits_[i]=0;
    return true;
}
void CompressedPagePool::Mark(uint32_t begin,uint32_t count,bool used) {
    while(count) {
        unsigned bit=begin%64,n=count<64-bit?count:64-bit;
        uint64_t mask=Mask(n)<<bit;
        if(used)bits_[begin/64]|=mask;else bits_[begin/64]&=~mask;
        begin+=n;count-=n;
    }
}
bool CompressedPagePool::Valid(uint32_t offset,uint16_t length) const {
    if(!bytes_ || !length || length>4096 || offset%Unit)return false;
    uint32_t at=offset/Unit,count=(length+Unit-1)/Unit;
    if(at>units_ || count>units_-at)return false;
    while(count) {
        unsigned bit=at%64,n=count<64-bit?count:64-bit;
        uint64_t mask=Mask(n)<<bit;if((bits_[at/64]&mask)!=mask)return false;
        at+=n;count-=n;
    }
    return true;
}
bool CompressedPagePool::Put(const void* source,uint16_t length,uint32_t& offset) {
    if(!bytes_ || !source || !length || length>4096)return false;
    const uint32_t count=(length+Unit-1)/Unit;
    if(count>units_-allocated_)return false;
    // A failed candidate advances beyond its first occupied unit. Fully used
    // words are skipped; the finite scan allocates no search records.
    for(uint32_t at=0;at+count<=units_;) {
        if(bits_[at/64]==~uint64_t{0}){at=(at/64+1)*64;continue;}
        uint32_t cursor=at,left=count;bool occupied=false;
        while(left) {
            unsigned bit=cursor%64,n=left<64-bit?left:64-bit;
            uint64_t used=(bits_[cursor/64]>>bit)&Mask(n);
            if(used){at=cursor+__builtin_ctzll(used)+1;occupied=true;break;}
            cursor+=n;left-=n;
        }
        if(occupied)continue;
        Mark(at,count,true);allocated_+=count;payload_+=length;offset=at*Unit;
        auto* from=static_cast<const uint8_t*>(source);
        for(unsigned i=0;i<length;++i)bytes_[offset+i]=from[i];
        return true;
    }
    return false;
}
const void* CompressedPagePool::Get(uint32_t offset,uint16_t length) const {
    return Valid(offset,length)?bytes_+offset:nullptr;
}
bool CompressedPagePool::Release(uint32_t offset,uint16_t length) {
    if(!Valid(offset,length))return false;
    uint32_t count=(length+Unit-1)/Unit;
    Mark(offset/Unit,count,false);allocated_-=count;payload_-=length;return true;
}
}
