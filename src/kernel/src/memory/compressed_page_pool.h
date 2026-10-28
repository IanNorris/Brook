#pragma once
#include <stdint.h>
namespace brook {
// Bounded resident blob arena; external serialization/lifetime required.
// Offsets are private slot metadata, never public identities. No allocations.
class CompressedPagePool {
public:
    static constexpr uint32_t Unit=64;
    bool Initialize(void* bytes,uint32_t capacity,uint64_t* bitmap,uint32_t words);
    bool Put(const void* bytes,uint16_t length,uint32_t& offset);
    const void* Get(uint32_t offset,uint16_t length) const;
    bool Release(uint32_t offset,uint16_t length);
    uint32_t Capacity() const{return units_*Unit;}
    uint32_t Allocated() const{return allocated_*Unit;}
    uint64_t Payload() const{return payload_;}
private:
    bool Valid(uint32_t offset,uint16_t length) const;
    void Mark(uint32_t begin,uint32_t units,bool used);
    uint8_t* bytes_=nullptr;
    uint64_t* bits_=nullptr;
    uint32_t units_=0,allocated_=0;
    uint64_t payload_=0;
};
}
