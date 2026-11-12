#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "panic_qr.h"
#include "panic_journal.h"
#include "intel_bdw_exec_snapshot.h"
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
    brook::PanicStackTrace trace{};trace.depth=255;trace.rip[0]=0x12345678;
    brook::PanicExceptionInfo exception{};
    brook::PanicProcessList processes{};processes.count=255;
    brook::PanicSystemInfo system{};
    brook::PanicStackDump stack{};stack.length=65535;
    brook::PanicCpuList cpus{};cpus.count=255;
    char message[brook::PANIC_MESSAGE_MAX_BYTES+9];std::memset(message,'X',sizeof(message));
    brook::BdwGpuCapture gpu{};gpu.version=3;gpu.count=1;gpu.requested=2;gpu.entries[0].address=0x12345000;
    gpu.entries[0].size=4096;gpu.entries[0].bytes=128;gpu.entries[0].data[0]=0xa5;
    brook::PanicSetGpuCapture(gpu);
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
    size_t at=0;bool found=false,foundLog=false,foundGpu=false;
    while(at+sizeof(header)<=raw.size()) {
        std::memcpy(&header,raw.data()+at,sizeof(header));at+=sizeof(header);
        assert(header.size<=raw.size()-at);
        if(header.type==brook::QR_PACKET_TYPE_STACK_TRACE)assert(uint8_t(raw[at])==brook::PANIC_MAX_STACK_DEPTH);
        if(header.type==brook::QR_PACKET_TYPE_PROCESS_LIST || header.type==brook::QR_PACKET_TYPE_PROCESS_EXT)
            assert(uint8_t(raw[at])==brook::PANIC_MAX_PROCESSES);
        if(header.type==brook::QR_PACKET_TYPE_CPU_STATE)assert(uint8_t(raw[at])==brook::PANIC_MAX_CPUS_DUMP);
        if(header.type==brook::QR_PACKET_TYPE_STACK_DUMP) {
            uint16_t size=0;std::memcpy(&size,raw.data()+at+8,2);
            assert(size==brook::PANIC_STACK_DUMP_BYTES && header.size==10+size);
        }
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
        if(header.type==brook::QR_PACKET_TYPE_GPU_CAPTURE){
            assert(!foundGpu && header.size==16+24+128);brook::BdwGpuCapture parsed{};
            std::memcpy(&parsed,raw.data()+at,header.size);
            assert(parsed.version==4 && parsed.count==1 && parsed.requested==2 && parsed.entries[0].address==0x12345000 && parsed.entries[0].data[0]==0xa5);foundGpu=true;
        }
        at+=header.size;
    }
    assert(found && foundLog && foundGpu && at==raw.size());
    // With a nearly full base report, retain as many marked samples as fit
    // and encode the actual lengths/count instead of dropping them all.
    gpu.count=4;gpu.requested=5;
    for(unsigned i=0;i<4;++i){gpu.entries[i].address=0x200000+i*4096;gpu.entries[i].size=4096;
        gpu.entries[i].bytes=512;std::memset(gpu.entries[i].data,int(0x30+i),512);}
    brook::PanicSetGpuCapture(gpu);brook::PanicPreparePacket(&regs,&trace,&exception,&processes,&system,&stack,&cpus,message);
    assert(brook::PanicCapturedPacket(&captured,&capturedBytes));raw.clear();
    if(captured[1]==brook::QR_VERSION){unsigned n=unsigned(captured[8])|(unsigned(captured[9])<<8)|(unsigned(captured[10])<<16)|(unsigned(captured[11])<<24);
        raw.resize(n);assert(LZ4_decompress_safe((const char*)captured+12,raw.data(),capturedBytes-12,n)==(int)n);
    }else raw.assign(captured+8,captured+capturedBytes);
    at=0;foundGpu=false;
    while(at+sizeof(header)<=raw.size()){
        std::memcpy(&header,raw.data()+at,sizeof(header));at+=sizeof(header);assert(header.size<=raw.size()-at);
        if(header.type==brook::QR_PACKET_TYPE_GPU_CAPTURE){
            uint32_t version=0,count=0,requested=0;std::memcpy(&version,raw.data()+at,4);std::memcpy(&count,raw.data()+at+4,4);
            std::memcpy(&requested,raw.data()+at+8,4);assert(version==4 && count>0 && count<=4 && requested==5);
            size_t cursor=at+16;for(unsigned i=0;i<count;++i){uint32_t length=0;assert(cursor+24<=at+header.size);
                std::memcpy(&length,raw.data()+cursor+16,4);assert(length<=512 && cursor+24+length<=at+header.size);
                for(unsigned j=0;j<length;++j)assert(raw[cursor+24+j]==char(0x30+i));cursor+=24+length;}
            assert(cursor==at+header.size);foundGpu=true;
        }
        at+=header.size;
    }
    assert(foundGpu && at==raw.size());
    // An incompressible full capture exceeds QR capacity. Preserve it in the
    // journal while serial/QR receive a separate complete core packet.
    uint32_t random=0x12345678;
    for(unsigned i=0;i<4;++i){gpu.entries[i].size=8192;gpu.entries[i].bytes=8192;
        for(unsigned j=0;j<8192;++j){random^=random<<13;random^=random>>17;random^=random<<5;gpu.entries[i].data[j]=uint8_t(random);}}
    brook::PanicSetGpuCapture(gpu);const auto beforeLarge=output.size();
    brook::PanicPreparePacket(&regs,&trace,&exception,&processes,&system,&stack,&cpus,message);
    assert(brook::PanicCapturedPacket(&captured,&capturedBytes) && capturedBytes>32768);
    raw.clear();
    if(captured[1]==brook::QR_VERSION){unsigned n=unsigned(captured[8])|(unsigned(captured[9])<<8)|(unsigned(captured[10])<<16)|(unsigned(captured[11])<<24);
        raw.resize(n);assert(LZ4_decompress_safe((const char*)captured+12,raw.data(),capturedBytes-12,n)==(int)n);
    }else raw.assign(captured+8,captured+capturedBytes);
    at=0;foundGpu=false;
    while(at+sizeof(header)<=raw.size()){
        std::memcpy(&header,raw.data()+at,sizeof(header));at+=sizeof(header);assert(header.size<=raw.size()-at);
        if(header.type==brook::QR_PACKET_TYPE_GPU_CAPTURE){
            uint32_t version=0,count=0;std::memcpy(&version,raw.data()+at,4);std::memcpy(&count,raw.data()+at+4,4);
            assert(version==4 && count==4);size_t cursor=at+16;
            for(unsigned i=0;i<4;++i){uint32_t length=0;assert(cursor+24<=at+header.size);
                std::memcpy(&length,raw.data()+cursor+16,4);assert(length==8192 && cursor+24+length<=at+header.size);
                assert(!std::memcmp(raw.data()+cursor+24,gpu.entries[i].data,length));cursor+=24+length;}
            assert(cursor==at+header.size);foundGpu=true;
        }
        at+=header.size;
    }
    assert(foundGpu && at==raw.size());
    assert(output.find("full capture is in disk journal; serial/QR contain core report",beforeLarge)!=std::string::npos);
    assert(brook::PanicJournalWrite(disk,captured,capturedBytes)==brook::PanicJournalResult::Ok);
    assert(brook::PanicJournalReadLatest(disk,recovered.data(),recovered.size(),record)==brook::PanicJournalResult::Ok && record.bytes==capturedBytes);
    assert(!std::memcmp(recovered.data(),captured,capturedBytes));
    const auto beforeLargeQr=output.size();brook::PanicRenderPreparedQR(framebuffer.data(),1920,1080,1920*4);
    assert(output.find("PANIC_B45_P0:",beforeLargeQr)!=std::string::npos);
    puts("headless panic packet: PASS");
}
