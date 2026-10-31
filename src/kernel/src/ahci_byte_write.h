#pragma once
#include <stdint.h>
#include <string.h>

namespace brook {
// Caller owns the device I/O lock for the entire call. The scratch buffer is
// at least 65536 bytes. Io::read/write transfer complete 512-byte sectors;
// a failed write has an uncertain outcome and must quarantine the device.
template<class Io>
int AhciByteWrite(Io& io, uint64_t offset, const void* input, uint64_t length,
                  uint64_t sectors, uint8_t* scratch) {
    if((!input && length) || !scratch || length>0x7fffffffu ||
       sectors>~uint64_t{0}/512) return -22;
    const uint64_t capacity=sectors*512;
    if(offset>capacity || length>capacity-offset)return -22;
    const auto* source=static_cast<const uint8_t*>(input);
    uint64_t done=0;
    while(done<length) {
        const uint64_t pos=offset+done;
        const uint32_t within=uint32_t(pos%512);
        const uint64_t remaining=length-done;
        const bool partial=within || remaining<512;
        const uint32_t count=partial ? 1 : uint32_t(remaining/512>128?128:remaining/512);
        const uint32_t bytes=partial ? uint32_t(remaining<512-within?remaining:512-within) : count*512;
        if(partial && !io.read(pos/512,1,scratch))return done?int(done):-5;
        memcpy(scratch+within,source+done,bytes);
        if(!io.write(pos/512,count,scratch))return done?int(done):-5;
        done+=bytes;
    }
    return int(done);
}
} // namespace brook
