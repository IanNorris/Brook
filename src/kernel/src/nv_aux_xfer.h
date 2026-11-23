#pragma once
// NVIDIA DP AUX channel transfer (auxgm200/padgm200 register sequence), as a
// pure template over register access so the step-4 DP trainer and the host
// tests can use it. It is the same sequence as the nv-aux bridge command in
// debug_hardware.cpp: pad to AUX mode, acquire (idle, request, grant), and per
// transaction: data, address, ctrl with type/size, kick, wait, status (W1C),
// with retries on timeout/DEFER; release drops the request.
// Io: uint32_t rd(uint32_t); void wr(uint32_t,uint32_t); void udelay(uint32_t us).
#include <stdint.h>
#include "nv_aux_reply.h"

namespace brook {
constexpr uint32_t kAuxNativeWrite=0x8, kAuxNativeRead=0x9;
template<typename Io> struct NvAux{
    Io& io;uint32_t ch;bool held=false;
    uint32_t B()const{return ch*0x50;}
    uint32_t R(uint32_t o){return io.rd(o+B());}
    void W(uint32_t o,uint32_t v){io.wr(o+B(),v);}
    // 0 ok, -16 busy (no idle), -17 no grant
    int Acquire(){
        W(0xd970,(R(0xd970)&~0xc003u)|0x2u);W(0xd97c,R(0xd97c)&~1u);       // pad -> AUX, enabled
        int i=0;for(;i<1000;++i){if(!(R(0xd954)&0x07010000))break;io.udelay(1);}
        if(i==1000)return -16;
        W(0xd954,(R(0xd954)&~0x00700000u)|0x00100000u);
        for(i=0;i<1000;++i){if((R(0xd954)&0x07000000)==0x01000000)break;io.udelay(1);}
        if(i==1000)return -17;
        held=true;return 0;
    }
    void Release(){W(0xd954,R(0xd954)&~0x00710000u);held=false;}
    // One transaction of up to 16 bytes. 0 ok, -5 I/O, -6 no sink / NACK, -11 retries exhausted.
    int Xfer(uint32_t type,uint32_t addr,uint8_t* data,uint32_t& size){
        if(size>16)size=16;
        if(!(R(0xd958)&0x10000000))return -6;
        for(uint32_t retries=0;retries<32;++retries){
            if(retries)io.udelay(400);
            if(!(type&1))for(uint32_t i=0;i<16;i+=4){uint32_t v=0;for(uint32_t b=0;b<4&&i+b<size;++b)v|=uint32_t(data[i+b])<<(8*b);W(0xd930+i,v);}
            W(0xd950,addr);
            uint32_t c=R(0xd954);c&=~0x0001f1ffu;c|=type<<12;c|=size?(size-1):0x100;
            W(0xd954,0x80000000u|c);W(0xd954,c);W(0xd954,0x00010000u|c);
            int t=2000;while((R(0xd954)&0x00010000)&&--t)io.udelay(1);
            if(!t)return -5;
            const uint32_t st=R(0xd958);W(0xd958,st);
            if(st&0x00000100)continue;              // timeout -> retry
            if(st&0x00000e00)return -5;
            const uint32_t reply=(st>>16)&0xf;
            if(reply==0x2||reply==0x8)continue;     // DEFER -> retry
            if(reply==0x1||reply==0x4)return -6;    // NACK
            if(type&1)size=NvAuxCopyReply(data,size,st&0x1f,[&](uint32_t o){return R(0xd940+o);});
            return 0;
        }
        return -11;
    }
    // Native DPCD access of any length (16-byte chunks); 0 ok.
    int Dpcd(bool read,uint32_t addr,uint8_t* data,uint32_t n){
        for(uint32_t done=0;done<n;){
            uint32_t chunk=n-done>16?16:n-done;const uint32_t want=chunk;
            const int rc=Xfer(read?kAuxNativeRead:kAuxNativeWrite,addr+done,data+done,chunk);
            if(rc)return rc;
            if(read&&chunk!=want)return -5;
            done+=want;
        }
        return 0;
    }
};
} // namespace brook
