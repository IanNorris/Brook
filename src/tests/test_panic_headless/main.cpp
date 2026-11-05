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
void SerialPanicPutChar(char c) {output += c;}
void SerialPanicPuts(const char* s) {output += s;}
int DebugOverlayTryPanicSnapshot(char* out,uint32_t capacity) {
    const char text[]="recent log\n";
    if(capacity<brook::PANIC_LOG_MAX_BYTES)return -22;
    std::memset(out,'L',brook::PANIC_LOG_MAX_BYTES);
    std::memcpy(out,text,sizeof(text)-1);return brook::PANIC_LOG_MAX_BYTES;
}
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
    brook::PanicStackTrace trace{};trace.depth=brook::PANIC_MAX_STACK_DEPTH;trace.rip[0]=0x12345678;
    brook::PanicExceptionInfo exception{};
    brook::PanicProcessList processes{};processes.count=brook::PANIC_MAX_PROCESSES;
    brook::PanicSystemInfo system{};
    brook::PanicStackDump stack{};stack.length=brook::PANIC_STACK_DUMP_BYTES;
    brook::PanicCpuList cpus{};cpus.count=brook::PANIC_MAX_CPUS_DUMP;
    char message[520];std::memset(message,'X',sizeof(message));
    brook::PanicRenderQR(nullptr,0,0,0,&regs,&trace,&exception,&processes,&system,&stack,&cpus,message);
    const auto pos=output.find("PANIC_HEX:"); assert(pos!=std::string::npos);
    const auto end=output.find('\n',pos);assert(end!=std::string::npos);
    std::vector<unsigned char> bytes;
    for(size_t i=pos+10;i<end;i+=2) {unsigned x=0;assert(sscanf(output.substr(i,2).c_str(),"%2x",&x)==1);bytes.push_back(x);}
    assert(brook::PanicCapturedPacket(&captured,&capturedBytes));
    assert(capturedBytes==bytes.size() && std::memcmp(captured,bytes.data(),bytes.size())==0);
    // The display path must render the already-captured packet without
    // rebuilding it or emitting a second independent PANIC_HEX record.
    std::vector<uint32_t> framebuffer(1920*1080,0);
    const size_t beforeRender=output.size();
    brook::PanicRenderPreparedQR(framebuffer.data(),1920,1080,1920*4);
    assert(output.find("PANIC_B45_P0:",beforeRender)!=std::string::npos);
    assert(output.find("PANIC_HEX:",beforeRender)==std::string::npos);
    bool drew=false;for(auto pixel:framebuffer)if(pixel){drew=true;break;}
    assert(drew && brook::PanicCapturedPacket(&captured,&capturedBytes));
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
    size_t at=0;bool found=false,foundLog=false;
    while(at+sizeof(header)<=raw.size()) {
        std::memcpy(&header,raw.data()+at,sizeof(header));at+=sizeof(header);
        assert(header.size<=raw.size()-at);
        if(header.type==brook::QR_PACKET_TYPE_MESSAGE) {
            assert(!found && header.size==brook::PANIC_MESSAGE_MAX_BYTES);
            for(size_t i=0;i<header.size;i++)assert(raw[at+i]=='X');
            found=true;
        }
        if(header.type==brook::QR_PACKET_TYPE_LOG_TAIL) {
            const char expected[]="\0recent log\n";
            assert(!foundLog && header.size==1+brook::PANIC_LOG_MAX_BYTES);
            assert(std::memcmp(raw.data()+at,expected,sizeof(expected)-1)==0);
            foundLog=true;
        }
        at+=header.size;
    }
    assert(found && foundLog && at==raw.size());
    puts("headless panic packet: PASS");
}
