#pragma once
#include <stdint.h>
namespace brook {
// Transport owns one bounded bounce buffer and serializes each whole operation.
// readBlocks/writeBlocks must complete exactly, or return false and quarantine
// its transport until outstanding DMA has been stopped.
template<class Io> int UsbBlockReadBytes(Io& io,uint64_t offset,void* out,uint64_t length) {
    if(!io.usable())return -5;
    const uint32_t block=io.blockSize(), capacity=io.bounceBytes();
    if(!block || (block&(block-1)) || block>capacity || capacity%block ||
       io.blockCount()>~uint64_t{0}/block || length>0x7fffffffu || (!out && length)) return -22;
    const uint64_t bytes=io.blockCount()*block;
    if(offset>bytes)return -22;
    if(length>bytes-offset)length=bytes-offset;
    auto* dst=static_cast<uint8_t*>(out);uint64_t done=0;
    while(done<length) {
        const uint32_t head=offset%block;
        uint64_t take=length-done;if(take>capacity-head)take=capacity-head;
        const uint32_t blocks=(head+static_cast<uint32_t>(take)+block-1)/block;
        if(offset/block>0xffffffffu || blocks>0x100000000ull-offset/block ||
           !io.readBlocks(offset/block,blocks)) return done?static_cast<int>(done):-5;
        for(uint32_t i=0;i<take;++i)dst[done+i]=io.bounce()[head+i];
        offset+=take;done+=take;
    }
    return static_cast<int>(done);
}
template<class Io> int UsbBlockWriteBytes(Io& io,uint64_t offset,const void* in,uint64_t length) {
    if(!io.usable())return -5;
    const uint32_t block=io.blockSize(),capacity=io.bounceBytes();
    if(!block || (block&(block-1)) || block>capacity || capacity%block ||
       io.blockCount()>~uint64_t{0}/block || length>0x7fffffffu || (!in && length)) return -22;
    const uint64_t bytes=io.blockCount()*block;
    if(offset>bytes || length>bytes-offset)return -22;
    auto* src=static_cast<const uint8_t*>(in);uint64_t done=0;
    while(done<length) {
        const uint32_t head=offset%block;
        uint64_t take=length-done;if(take>capacity-head)take=capacity-head;
        const uint32_t blocks=(head+static_cast<uint32_t>(take)+block-1)/block;
        const uint64_t lba=offset/block;
        if(lba>0xffffffffu || blocks>0x100000000ull-lba)return done?static_cast<int>(done):-5;
        if((head || (head+take)%block) && !io.readBlocks(lba,blocks))return done?static_cast<int>(done):-5;
        for(uint32_t i=0;i<take;++i)io.bounce()[head+i]=src[done+i];
        if(!io.writeBlocks(lba,blocks))return done?static_cast<int>(done):-5;
        offset+=take;done+=take;
    }
    return static_cast<int>(done);
}
}
