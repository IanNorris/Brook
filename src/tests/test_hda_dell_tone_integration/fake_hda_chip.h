#pragma once
#include <cassert>
#include <cstdint>
#include "../../drivers/intel_hda/hda_probe_session.h"
using namespace brook;
struct Chip {
    uint8_t mem[4096]{};uint8_t pin[2]{0,0},dac[2]{127,127},mix[2]{0,0},selection=0,converter=0;
    uint16_t format=0x4011,runConverterFormat=0,runStreamFormat=0;uint64_t time=0;uint32_t cmd=0,response=0,pos=0;unsigned statusReads=0,failStatusAfter=0;bool published=false,quarantined=false,released=false,dropBdlClear=false,sawQuietUnmute=false,snapshotMode=false,dropStop=false,corruptNewFormat=false,roPriority=false,needsReset=false,resetAsserted=false,dropRetireReset=false,reportEndpoint=false,sawEndpoint=false,dropPinRestoreRight=false;
    static constexpr uint32_t base=0x100;
    uint8_t* amp(unsigned node,bool output){if(node==0x14 && output)return pin;if(node==3 && output)return dac;if(node==0x0d && !output)return mix;assert(false);return nullptr;}
    void execute(){
        const unsigned n=(cmd>>20)&127,verb=(cmd>>8)&0xfff,shortVerb=(cmd>>16)&15,payload=cmd&0xffff;
        if(snapshotMode && verb==0xf00 && n==0 && (payload&0xff)==0){response=0x10ec0288;return;}
        if(snapshotMode && verb==0xf00 && n==0 && (payload&0xff)==2){response=0x100003;return;}
        if(snapshotMode && verb==0xf20 && n==1){response=0x10280665;return;}
        if(snapshotMode && verb!=0xf01 && verb!=0xf06){
            const uint32_t values[]={0x40050d,0x10010,0x90170110,2,0x0d0c,0,0x40,2,0x20010b,1,3,0x41d};
            const uint32_t states[]={0x400,0xe0060,1,0,0,0x400,0,0xe0060,1,0x17f7f,0x80000000,0,0x80000000};
            for(unsigned i=0;i<12;++i){const auto& q=HdaDellSpeakerQueries[i];if(n==q.nid && verb==q.verb && (payload&0xff)==q.payload){response=values[i];return;}}
            for(unsigned i=0;i<13;++i){const auto& q=HdaDellStateQueries[i];if(n==q.nid && verb==q.verb && (payload&0xff)==q.payload){response=states[i];return;}}
        }
        if(shortVerb==3){const bool output=payload&0x8000,left=payload&0x2000,right=payload&0x1000;assert(left!=right);
            if(n!=0x14)assert(pin[0]==0x80 && pin[1]==0x80);
            if(n==0x14 && uint8_t(payload)==0 && (read16(base)&2)){assert(dac[0]<=80 && dac[1]<=80);sawQuietUnmute=true;}
            if(!(dropPinRestoreRight && n==0x14 && output && right && uint8_t(payload)==0 && !(read16(base)&2) && converter==0 && pin[0]==0)) {
                amp(n,output)[left?0:1]=uint8_t(payload);
            }
            response=0;}
        else if(shortVerb==0xb){const bool output=payload&0x8000,left=payload&0x2000;response=amp(n,output)[left?0:1];}
        else if(shortVerb==2){assert(n==3);format=uint16_t(payload);response=0;}
        else if(shortVerb==0xa){assert(n==3);response=corruptNewFormat && format==0x11?0x10:format;}
        else if(verb==0x701){assert(n==0x14 && pin[0]==0x80 && pin[1]==0x80);selection=uint8_t(payload);response=0;}
        else if(verb==0xf01){assert(n==0x14);response=selection;}
        else if(verb==0x706){assert(n==3);converter=uint8_t(payload);response=0;}
        else if(verb==0xf06){assert(n==3);response=converter;}
        else assert(false);
    }
    uint8_t read8(uint32_t o){if(o==base+3 && failStatusAfter && ++statusReads>=failStatusAfter)return uint8_t(mem[o]|8);return mem[o];}
    uint16_t read16(uint32_t o){return uint16_t(mem[o])|(uint16_t(mem[o+1])<<8);}
    uint32_t read32(uint32_t o){if(o==0x64)return response;if(o==base+4 && (read16(base)&2)){pos=(pos+192)%12288;if(!pos && reportEndpoint){sawEndpoint=true;return 12288;}return pos;}
        return uint32_t(read16(o))|(uint32_t(read16(o+2))<<16);}
    void write8(uint32_t o,uint8_t v){if(o==base+3)mem[o]&=~v;else mem[o]=uint8_t(v|((o==base+2 && roPriority)?4:0));}
    void write16(uint32_t o,uint16_t v){if(o==0x68){if(v==2)mem[o]&=~2;else if(v==1){execute();mem[o]=2;}return;}
        if(dropStop && o==base && v==0 && (read16(base)&2))return;
        if(o==base && v==1){if(dropRetireReset && needsReset)return;resetAsserted=true;}
        if(o==base && v==0 && resetAsserted){needsReset=false;resetAsserted=false;}
        if(o==base && v==2){assert(published && pin[0]==0x80 && pin[1]==0x80);runConverterFormat=format;runStreamFormat=read16(base+0x12);needsReset=true;}mem[o]=v;mem[o+1]=v>>8;}
    void write32(uint32_t o,uint32_t v){if(o==0x60){cmd=v;return;}if(o==base+8)assert(!needsReset);if(dropBdlClear && o==base+0x18 && !v)return;for(unsigned i=0;i<4;++i)mem[o+i]=v>>(8*i);}
    uint64_t ticks(){return time++;}void pause(){}bool delayUs(unsigned n){return n>=3;}bool publishDma(){published=true;return true;}
};
inline HdaSessionReport makeSnapshot(){
    HdaSessionReport s{};s.result=HdaSessionResult::Complete;s.probe=HdaProbeResult::Ready;s.vendor=0x10ec0288;s.subsystem=0x10280665;
    s.completed=12;s.stateCompleted=13;s.ampCompleted=6;s.formatCompleted=true;s.converterFormat=0x4011;s.ampValues[0]=s.ampValues[1]=127;
    const uint32_t v[]={0x40050d,0x10010,0x90170110,2,0x0d0c,0,0x40,2,0x20010b,1,3,0x41d};
    const uint32_t p[]={0x400,0xe0060,1,0,0,0x400,0,0xe0060,1,0x17f7f,0x80000000,0,0x80000000};
    for(unsigned i=0;i<12;++i)s.values[i]=v[i];for(unsigned i=0;i<13;++i)s.stateValues[i]=p[i];
    s.controller.gcap=0x4401;return s;
}
