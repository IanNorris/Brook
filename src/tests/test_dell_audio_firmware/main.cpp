#include "../../kernel/src/dell_audio_firmware.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace brook;
struct Io {
    uint8_t pn[0xe4]{},gn[0x35b]{};bool denied=false,fail=false,change=false,any=false;unsigned calls=0;
    bool containsNvs(uint64_t p,uint32_t n){return !denied && (any || (p==0x1000 && n==sizeof(pn)) || (p==0x2000 && n==sizeof(gn)));}
    bool readNvs(uint64_t p,void* out,uint32_t n){
        if(fail)return false;
        if(any){std::memset(out,0,n);return true;}
        const uint8_t* src=nullptr;
        if(p>=0x1000 && p-0x1000<=sizeof(pn) && n<=sizeof(pn)-(p-0x1000))src=pn+(p-0x1000);
        if(p>=0x2000 && p-0x2000<=sizeof(gn) && n<=sizeof(gn)-(p-0x2000))src=gn+(p-0x2000);
        if(!src)return false;std::memcpy(out,src,n);if(change && calls>=10)static_cast<uint8_t*>(out)[0]^=1;++calls;return true;
    }
};
static std::vector<uint8_t> fixture(){
    std::vector<uint8_t> d(512);std::memcpy(d.data(),"DSDT",4);BrookUpdatePut32(d.data()+4,d.size());
    const uint8_t pn[]={0x08,'P','N','V','B',0x0c,0,0x10,0,0,0x08,'P','N','V','L',0x0b,0xe4,0,
        0x5b,0x80,'P','N','V','A',0,'P','N','V','B','P','N','V','L'};
    const uint8_t gn[]={0x5b,0x80,'G','N','V','S',0,0x0c,0,0x20,0,0,0x0b,0x5b,3};
    std::memcpy(d.data()+64,pn,sizeof(pn));std::memcpy(d.data()+128,gn,sizeof(gn));return d;
}
int main(int argc,char** argv){
    auto d=fixture();Io io;DellAudioFirmwareSnapshot out{};uint8_t digest[32];
    auto run=[&](){BrookUpdateHash(d.data(),d.size(),digest);return DellAudioFirmwareCapture(d.data(),d.size(),digest,io,out);};
    io.gn[0]=0xdd;io.gn[1]=7;io.gn[284]=1;io.pn[6]=2;io.pn[114]=1;io.pn[122]=9;
    BrookUpdatePut32(io.pn+44,0x12340000);BrookUpdatePut32(io.pn+48,0x12350000);
    BrookUpdatePut32(io.pn+52,7);BrookUpdatePut32(io.pn+133,0x12360000);BrookUpdatePut32(io.pn+165,0x12370000);
    assert(run()==DellAudioFirmwareResult::Ok && out.osys==0x7dd && out.codecSelection==1 && out.pchGeneration==2);
    assert(out.dspBase0==0x12340000 && out.dspBase1==0x12350000 && out.dspIrq==7 && out.i2cMode==1 && out.i2cIrq==9);
    assert(out.i2cBase0==0x12360000 && out.i2cBase1==0x12370000 && io.calls==20);
    digest[0]^=1;assert(DellAudioFirmwareCapture(d.data(),d.size(),digest,io,out)==DellAudioFirmwareResult::HashMismatch);
    io.denied=true;assert(run()==DellAudioFirmwareResult::Nvs);io.denied=false;
    io.fail=true;assert(run()==DellAudioFirmwareResult::Nvs);io.fail=false;
    io.calls=0;io.change=true;assert(run()==DellAudioFirmwareResult::Unstable);io.change=false;
    d[64+16]=0xe3;assert(run()==DellAudioFirmwareResult::Nvs);d[64+16]=0xe4;io.calls=0;assert(run()==DellAudioFirmwareResult::Ok);
    std::memcpy(d.data()+256,d.data()+64,10);assert(run()==DellAudioFirmwareResult::Layout);
    if(argc==2){
        std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> actual((std::istreambuf_iterator<char>(f)),{});
        assert(f.eof() || f.good());assert(!actual.empty());io.any=true;BrookUpdateHash(actual.data(),actual.size(),digest);
        assert(DellAudioFirmwareCapture(actual.data(),actual.size(),digest,io,out)==DellAudioFirmwareResult::Ok);
    }else assert(argc==1);
}
