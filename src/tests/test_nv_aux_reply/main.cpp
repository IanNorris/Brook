#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "nv_aux_reply.h"
using namespace brook;

static const uint32_t kWords[4]={0x04030201,0x08070605,0x0c0b0a09,0x100f0e0d};
static uint32_t Word(uint32_t off){return kWords[off/4];}

// A 1-byte buffer with guards on both sides: the DPCD 0x600 readback that
// smashed the debug thread's stack (eddd24a7 panic, 22 Nov).
static void OneByte(){
    uint8_t mem[1+16+1];memset(mem,0xaa,sizeof(mem));
    uint8_t* back=mem+1;
    assert(NvAuxCopyReply(back,1,16,Word)==1);
    assert(back[0]==0x01);
    for(uint32_t i=1;i<sizeof(mem)-1;++i)assert(back[i]==0xaa);
    assert(mem[0]==0xaa);
}
// Lane status (6 bytes) and lane set (4 bytes): unaligned tail inside a word.
static void Short(){
    uint8_t st[6+10];memset(st,0xaa,sizeof(st));
    assert(NvAuxCopyReply(st,6,16,Word)==6);
    for(uint32_t i=0;i<6;++i)assert(st[i]==i+1);
    for(uint32_t i=6;i<sizeof(st);++i)assert(st[i]==0xaa);
    uint8_t conf[4+12];memset(conf,0xaa,sizeof(conf));
    assert(NvAuxCopyReply(conf,4,4,Word)==4);
    assert(conf[3]==4&&conf[4]==0xaa);
}
// Sink returned fewer bytes than asked: copy only those.
static void ShortReply(){
    uint8_t buf[16];memset(buf,0xaa,sizeof(buf));
    assert(NvAuxCopyReply(buf,16,3,Word)==3);
    assert(buf[2]==3&&buf[3]==0xaa);
    assert(NvAuxCopyReply(buf,8,0,Word)==0);
}
// Full window and over-asks clamp to 16.
static void Full(){
    uint8_t buf[32];memset(buf,0xaa,sizeof(buf));
    assert(NvAuxCopyReply(buf,16,16,Word)==16);
    for(uint32_t i=0;i<16;++i)assert(buf[i]==i+1);
    memset(buf,0xaa,sizeof(buf));
    assert(NvAuxCopyReply(buf,32,31,Word)==16);
    assert(buf[15]==16&&buf[16]==0xaa);
}
int main(){OneByte();Short();ShortReply();Full();puts("test_nv_aux_reply: ok");return 0;}
