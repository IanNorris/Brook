#include <cassert>
#include <cstring>
#include <fstream>
#include <string>
#include "writable_desktop_plan.h"
using namespace brook;
namespace {
constexpr const char* Record="BRKSTORE1 16777216 cf16f1ae-68c5-5fac-89e9-382b8af5cd5c\n";
void Put16(uint8_t* p,uint16_t n){p[0]=n;p[1]=n>>8;}
void Put32(uint8_t* p,uint32_t n){Put16(p,n);Put16(p+2,n>>16);}
struct Disk {
    uint8_t sb[1024]{},gdt[1024]{};
    int readError=0,writeError=0,flushError=0;
    bool shortRead=false,shortWrite=false;
    unsigned reads=0,writes=0,flushes=0;
    Disk(){
        WritableDesktopPlan plan;assert(WritableDesktopPlanParse(Record,std::strlen(Record),plan));
        Put32(sb,1024);Put32(sb+4,4096);Put32(sb+12,4000);Put32(sb+16,1000);
        Put32(sb+24,2);Put32(sb+28,2);Put32(sb+32,32768);Put32(sb+36,32768);Put32(sb+40,1024);
        Put16(sb+56,0xef53);Put16(sb+58,1);Put32(sb+76,1);Put32(sb+84,11);Put16(sb+88,128);
        Put32(sb+96,2);Put32(sb+100,3);std::memcpy(sb+104,plan.uuid,16);
        Put32(gdt,2);Put32(gdt+4,3);Put32(gdt+8,4);
    }
    WritableDesktopIo Io(){return {this,
        [](void* p,uint64_t off,void* out,uint32_t n){auto& d=*static_cast<Disk*>(p);++d.reads;
            if(d.readError)return d.readError;if(d.shortRead)return int(n-1);
            if(off==1024 && n==1024)std::memcpy(out,d.sb,n);
            else {assert(off==4096 && n<=1024);std::memcpy(out,d.gdt,n);}return int(n);},
        [](void* p,uint64_t off,const void* data,uint32_t n){auto& d=*static_cast<Disk*>(p);++d.writes;
            assert(off==1082 && n==2);if(d.writeError)return d.writeError;
            std::memcpy(d.sb+58,data,d.shortWrite?1:2);return d.shortWrite?1:2;},
        [](void* p){auto& d=*static_cast<Disk*>(p);++d.flushes;return d.flushError;}};}
};
}
int main(int argc,char** argv){
    WritableDesktopPlan plan;assert(WritableDesktopPlanParse(Record,std::strlen(Record),plan));
    assert(plan.bytes==16777216 && plan.uuid[0]==0xcf);
    for(const std::string bad:{"", "BRKSTORE1 4294967296 cf16f1ae-68c5-5fac-89e9-382b8af5cd5c\n",
        "BRKSTORE1 16777217 cf16f1ae-68c5-5fac-89e9-382b8af5cd5c\n",
        "BRKSTORE1 16777216 00000000-0000-0000-0000-000000000000\n",
        "BRKSTORE1 16777216 CF16f1ae-68c5-5fac-89e9-382b8af5cd5c\n",
        "BRKSTORE1 16777216 cf16f1ae-68c5-5fac-89e9-382b8af5cd5c\nextra"}){
        WritableDesktopPlan out;assert(!WritableDesktopPlanParse(bad.c_str(),bad.size(),out));
    }
    Disk d;assert(WritableDesktopHeaderValid(d.sb,plan));assert(WritableDesktopGroupsValid(d.sb,d.gdt,32));
    const unsigned offsets[]={0,4,8,15,19,20,24,28,32,36,40,56,58,72,76,84,88,90,92,96,100,104};
    for(unsigned at:offsets){
        Disk invalid;invalid.sb[at]^=0x80;WritableDesktopSession s;
        assert(s.Begin(invalid.Io(),plan)<0 && !invalid.writes);
    }
    for(unsigned bad=0;bad<5;++bad){
        Disk invalid;if(bad==0)Put32(invalid.gdt,1);if(bad==1)Put32(invalid.gdt+4,2);
        if(bad==2)Put32(invalid.gdt+8,4090);if(bad==3)Put32(invalid.gdt+8,2);
        if(bad==4)Put16(invalid.gdt+12,4097);
        WritableDesktopSession s;assert(s.Begin(invalid.Io(),plan)<0 && !invalid.writes);
    }
    for(unsigned fault=0;fault<4;++fault){
        Disk broken;if(fault==0)broken.readError=-5;if(fault==1)broken.shortRead=true;
        if(fault==2)broken.shortWrite=true;if(fault==3)broken.flushError=-5;
        WritableDesktopSession s;assert(s.Begin(broken.Io(),plan)<0 && !s.Arm());
        const auto written=broken.writes;assert(s.Finish(broken.Io(),true)==0 && broken.writes==written);
    }
    WritableDesktopSession s;assert(s.Begin(d.Io(),plan)==0 && s.dirty && !StoreGet16(d.sb+58));
    assert(d.reads==2 && d.writes==1 && d.flushes==1);
    // Failed admission/interrupted boot cannot be made clean by later global
    // shutdown. A fresh boot refuses the dirty image before another write.
    assert(s.Finish(d.Io(),true)==0 && !StoreGet16(d.sb+58));
    WritableDesktopSession next;assert(next.Begin(d.Io(),plan)<0 && d.writes==1);
    assert(s.Arm());assert(s.Finish(d.Io(),false)==-1 && !StoreGet16(d.sb+58));
    assert(s.Finish(d.Io(),true)==0 && StoreGet16(d.sb+58)==1 && s.clean);
    auto writes=d.writes;assert(s.Finish(d.Io(),true)==0 && d.writes==writes);
    assert(next.Begin(d.Io(),plan)==0 && !StoreGet16(d.sb+58));
    for(unsigned fault=0;fault<3;++fault){
        Disk broken;WritableDesktopSession session;assert(!session.Begin(broken.Io(),plan) && session.Arm());
        if(fault==0)broken.writeError=-30;if(fault==1)broken.shortWrite=true;if(fault==2)broken.flushError=-5;
        assert(session.Finish(broken.Io(),true)<0 && !session.clean);
    }
    if(argc==3){
        // Read-only validation of an actual mke2fs-produced image and its
        // emitted boot record, independent of the synthetic corruption model.
        std::ifstream record(argv[2]);std::string text((std::istreambuf_iterator<char>(record)),{});
        WritableDesktopPlan actual;assert(WritableDesktopPlanParse(text.data(),text.size(),actual));
        std::ifstream image(argv[1],std::ios::binary);uint8_t sb[1024],gdt[1024];
        image.seekg(1024);assert(image.read(reinterpret_cast<char*>(sb),sizeof(sb)));
        assert(WritableDesktopHeaderValid(sb,actual));
        const unsigned n=((StoreGet32(sb+4)+32767)/32768)*32;
        image.seekg(4096);assert(image.read(reinterpret_cast<char*>(gdt),n));
        assert(WritableDesktopGroupsValid(sb,gdt,n));
    }
}
