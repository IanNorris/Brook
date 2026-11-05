#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "panic_qr.h"
#include "panic_journal.h"
#include "lz4.h"
static std::string output;
namespace brook {
extern "C" void SerialPuts(const char* s) {output += s;}
extern "C" void SerialPutChar(char c) {output += c;}
extern "C" void SerialPrintf(const char*, ...) {}
}
struct Disk {
    std::vector<uint8_t> sectors=std::vector<uint8_t>(brook::PanicJournalSectors*brook::PanicJournalSector);
    bool read(uint32_t n,uint8_t* out) {if(n>=brook::PanicJournalSectors)return false;std::memcpy(out,sectors.data()+n*512,512);return true;}
    bool write(uint32_t n,const uint8_t* in) {if(n>=brook::PanicJournalSectors)return false;std::memcpy(sectors.data()+n*512,in,512);return true;}
    bool flush() {return true;}
};
int main() {
    const uint8_t* captured=nullptr;uint32_t capturedBytes=0;
    assert(!brook::PanicCapturedPacket(&captured,&capturedBytes));
    brook::PanicCPURegs regs{}; regs.rip=0x12345678; regs.rsp=0x87654321;
    brook::PanicStackTrace trace{};trace.depth=1;trace.rip[0]=0x12345678;
    char message[520];std::memset(message,'X',sizeof(message));
    brook::PanicRenderQR(nullptr,0,0,0,&regs,&trace,nullptr,nullptr,nullptr,nullptr,nullptr,message);
    const auto pos=output.find("PANIC_HEX:"); assert(pos!=std::string::npos);
    const auto end=output.find('\n',pos);assert(end!=std::string::npos);
    std::vector<unsigned char> bytes;
    for(size_t i=pos+10;i<end;i+=2) {unsigned x=0;assert(sscanf(output.substr(i,2).c_str(),"%2x",&x)==1);bytes.push_back(x);}
    assert(brook::PanicCapturedPacket(&captured,&capturedBytes));
    assert(capturedBytes==bytes.size() && std::memcmp(captured,bytes.data(),bytes.size())==0);
    Disk disk;
    assert(brook::PanicJournalWrite(disk,captured,capturedBytes)==brook::PanicJournalResult::Ok);
    std::vector<uint8_t> recovered(brook::PanicJournalPayloadMax);
    brook::PanicJournalRecord record;
    assert(brook::PanicJournalReadLatest(disk,recovered.data(),recovered.size(),record)==brook::PanicJournalResult::Ok);
    assert(record.bytes==capturedBytes && std::memcmp(recovered.data(),captured,capturedBytes)==0);
    assert(bytes.size()>8 && bytes[0]==brook::QR_MAGIC_BYTE && bytes[2]==0 && bytes[3]==1);
    std::vector<char> raw;
    if(bytes[1]==brook::QR_VERSION) {
        assert(bytes.size()>12);unsigned n=unsigned(bytes[8])|(unsigned(bytes[9])<<8)|(unsigned(bytes[10])<<16)|(unsigned(bytes[11])<<24);
        raw.resize(n);assert(LZ4_decompress_safe((char*)bytes.data()+12,raw.data(),bytes.size()-12,n)==(int)n);
    } else {assert(bytes[1]==brook::QR_VERSION_RAW);raw.assign(bytes.begin()+8,bytes.end());}
    assert(raw.size()>=sizeof(brook::PanicPacketHeader)+sizeof(regs));
    brook::PanicPacketHeader header{};memcpy(&header,raw.data(),sizeof(header));
    assert(header.type==brook::QR_PACKET_TYPE_CPU_REGS && header.size==sizeof(regs));
    brook::PanicCPURegs decoded{};memcpy(&decoded,raw.data()+sizeof(header),sizeof(decoded));
    assert(decoded.rip==regs.rip && decoded.rsp==regs.rsp);
    size_t at=0;bool found=false;
    while(at+sizeof(header)<=raw.size()) {
        std::memcpy(&header,raw.data()+at,sizeof(header));at+=sizeof(header);
        assert(header.size<=raw.size()-at);
        if(header.type==brook::QR_PACKET_TYPE_MESSAGE) {
            assert(!found && header.size==brook::PANIC_MESSAGE_MAX_BYTES);
            for(size_t i=0;i<header.size;i++)assert(raw[at+i]=='X');
            found=true;
        }
        at+=header.size;
    }
    assert(found && at==raw.size());
    puts("headless panic packet: PASS");
}
