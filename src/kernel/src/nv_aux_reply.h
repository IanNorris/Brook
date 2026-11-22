#pragma once
#include <stdint.h>
namespace brook {
// NVIDIA DP AUX (auxgm200) native/I2C read reply copy. The controller's read
// window is four 32-bit words (16 bytes, little-endian); the status register
// reports how many bytes the sink returned. Copies at most min(want, 16,
// returned) bytes into data and returns that count: callers pass buffers as
// small as one byte, so the copy must never fill the whole window.
template<typename ReadWord>
inline uint32_t NvAuxCopyReply(uint8_t* data,uint32_t want,uint32_t returned,ReadWord readWord){
    if(want>16)want=16;
    const uint32_t n=returned<want?returned:want;
    for(uint32_t i=0;i<n;++i)data[i]=uint8_t(readWord(i&~3u)>>(8*(i&3)));
    return n;
}
}
