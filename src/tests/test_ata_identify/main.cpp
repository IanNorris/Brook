#include "../../kernel/src/ata_identify.h"
#include <cassert>
#include <cstring>
using namespace brook;
static void word(uint8_t* p,unsigned n,uint16_t v){p[n*2]=uint8_t(v);p[n*2+1]=uint8_t(v>>8);}
static void text(uint8_t* p,unsigned n,const char* s,unsigned len){for(unsigned i=0;i<len;i+=2)word(p,n+i/2,(uint16_t(s[i])<<8)|uint8_t(s[i+1]));}
int main(){
    uint8_t raw[512]{};AtaIdentifyInfo info{};
    assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::NoLba48);
    word(raw,83,0x4400);word(raw,100,0x1234);word(raw,101,1);
    text(raw,27,"TEST SSD                                ",40);
    assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::Ok);
    assert(info.sectors==0x11234 && info.logicalSectorBytes==512 && info.physicalSectorBytes==512);
    assert(!std::strcmp(info.model,"TEST SSD"));
    word(raw,106,0x6003);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::Ok && info.physicalSectorBytes==4096);
    word(raw,106,0x5000);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::UnsupportedSectorSize);
    word(raw,106,0);word(raw,0,0x8000);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::PacketDevice);
    word(raw,0,4);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::Incomplete);
    word(raw,0,0);word(raw,100,0);word(raw,101,0);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::InvalidCapacity);
    word(raw,83,0x8400);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::NoLba48);
    word(raw,83,0x4400);word(raw,103,1);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::Ok && info.sectors==(1ull<<48));
    word(raw,100,1);assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::InvalidCapacity);
    word(raw,103,0);word(raw,100,1);word(raw,106,0xd003);
    assert(AtaParseIdentify(raw,512,info)==AtaIdentifyResult::Ok && info.logicalSectorBytes==512 && info.physicalSectorBytes==512);
    assert(AtaParseIdentify(nullptr,512,info)==AtaIdentifyResult::Truncated);
    assert(AtaParseIdentify(raw,511,info)==AtaIdentifyResult::Truncated);
}
