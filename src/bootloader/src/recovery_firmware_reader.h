#pragma once
#include <stdint.h>

namespace brook { namespace bootloader {
// Read-only adapter for EFI_BLOCK_IO_PROTOCOL. Protocol is templated so the
// exact bounds, alignment and media-change behavior can be tested with a fake
// protocol. The caller owns an aligned, disjoint bounce buffer and must have
// selected the unique physical parent of the loaded ESP independently.
template<class Protocol>
class RecoveryFirmwareReader {
    Protocol* device;
    uint8_t* bounce;
    uint32_t bounceBytes, mediaId, blockSize, alignment;
    uint64_t lastBlock;
    bool valid;
public:
    RecoveryFirmwareReader(Protocol* d,uint8_t* b,uint32_t bytes)
      : device(d),bounce(b),bounceBytes(bytes),mediaId(0),blockSize(0),alignment(0),lastBlock(0),valid(false) {
        if(!d || !d->Media || !d->ReadBlocks || !b || !bytes)return;
        auto* m=d->Media;
        if(!m->MediaPresent || m->LogicalPartition || m->BlockSize!=512 ||
           m->LastBlock==~uint64_t{0} || m->LastBlock<67 ||
           (m->IoAlign>1 && ((m->IoAlign & (m->IoAlign-1)) || m->IoAlign>4096)) ||
           (m->IoAlign>1 && (reinterpret_cast<uintptr_t>(b)&(m->IoAlign-1))))return;
        mediaId=m->MediaId;blockSize=m->BlockSize;alignment=m->IoAlign;lastBlock=m->LastBlock;valid=true;
    }
    bool Stable() const {
        if(!valid || !device || !device->Media)return false;
        const auto* m=device->Media;
        return m->MediaPresent && !m->LogicalPartition && m->MediaId==mediaId &&
               m->BlockSize==blockSize && m->IoAlign==alignment && m->LastBlock==lastBlock;
    }
    uint64_t Sectors() const {return valid ? lastBlock+1 : 0;}
    bool Read(uint64_t lba,uint32_t sectors,void* out) {
        if(!Stable() || !out || !sectors || lba>lastBlock || uint64_t(sectors)>lastBlock-lba+1 ||
           uint64_t(sectors)*512>bounceBytes)return false;
        const uintptr_t dst=reinterpret_cast<uintptr_t>(out),src=reinterpret_cast<uintptr_t>(bounce);
        const uintptr_t length=uintptr_t(sectors)*512;
        if(dst>~uintptr_t{0}-length || src>~uintptr_t{0}-length ||
           (dst<src+length && src<dst+length))return false;
        if(device->ReadBlocks(device,mediaId,lba,length,bounce)!=0 || !Stable())return false;
        auto* target=static_cast<uint8_t*>(out);
        for(uintptr_t i=0;i<length;++i)target[i]=bounce[i];
        return true;
    }
};
}} // namespace brook::bootloader
