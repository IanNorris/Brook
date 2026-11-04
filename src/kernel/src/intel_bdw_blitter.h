#pragma once
#include <stdint.h>

namespace brook {
// This builds a single linear 32-bit copy, not a complete GPU submission.
// Addresses are GPU virtual addresses, not CPU or physical addresses. The
// executor must establish ownership, non-aliasing backing, synchronization,
// cache policy and lifetime before allowing this packet to reach hardware.
struct BdwBlitBuffer {
    uint64_t gpuAddress;
    uint64_t allocationBytes;
    uint32_t pitchBytes;
};
struct BdwBlitCopy {
    BdwBlitBuffer source, destination;
    uint32_t widthPixels, heightPixels;
};
inline bool BdwEncodeLinearCopy(const BdwBlitCopy& copy, uint32_t (&out)[10]) {
    const uint64_t limit = uint64_t{1} << 32;
    if (!copy.widthPixels || !copy.heightPixels || copy.widthPixels>32767 || copy.heightPixels>32767)
        return false;
    const auto fits=[&](const BdwBlitBuffer& b) {
        if ((b.gpuAddress & 63) || b.gpuAddress>=limit || !b.allocationBytes ||
            b.allocationBytes>limit-b.gpuAddress || !b.pitchBytes ||
            b.pitchBytes>=32768 || (b.pitchBytes&3)) return false;
        const uint64_t rowBytes=uint64_t(copy.widthPixels)*4;
        return rowBytes<=b.pitchBytes &&
            uint64_t(copy.heightPixels-1)*b.pitchBytes+rowBytes<=b.allocationBytes;
    };
    if (!fits(copy.source) || !fits(copy.destination)) return false;
    const auto& s=copy.source;const auto& d=copy.destination;
    if (s.gpuAddress<d.gpuAddress+d.allocationBytes && d.gpuAddress<s.gpuAddress+s.allocationBytes)
        return false;
    const uint32_t packet[10]={
        0x54f00008u, 0x03cc0000u | d.pitchBytes, 0,
        (copy.heightPixels<<16) | copy.widthPixels,
        static_cast<uint32_t>(d.gpuAddress),static_cast<uint32_t>(d.gpuAddress>>32),
        0,s.pitchBytes,static_cast<uint32_t>(s.gpuAddress),static_cast<uint32_t>(s.gpuAddress>>32)
    };
    for (unsigned i=0;i<10;++i) out[i]=packet[i];
    return true;
}
}
