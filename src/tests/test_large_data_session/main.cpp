#include <cassert>
#include <cstring>
#include "large_data_session.h"
using namespace brook;
static void put16(uint8_t* p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
static void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
struct Disk {
    uint8_t sb[1024]{},gdt[32]{},uuid[16]{};
    unsigned reads=0,writes=0,flushes=0;int failRead=0,failWrite=0,failFlush=0,shortRead=0,shortWrite=0;
    bool ignoreWrite=false;
    Disk(){
        put32(sb,1024);put32(sb+4,4096);put32(sb+12,4000);put32(sb+16,1000);
        put32(sb+24,2);put32(sb+28,2);put32(sb+32,32768);put32(sb+36,32768);put32(sb+40,1024);
        put16(sb+56,0xef53);put16(sb+58,1);put32(sb+76,1);put32(sb+84,11);put16(sb+88,128);
        put32(sb+96,2);put32(sb+100,3);uuid[0]=4;std::memcpy(sb+104,uuid,16);
        put32(gdt,2);put32(gdt+4,3);put32(gdt+8,4);
    }
    WritableDesktopIo Io(){return {this,
        [](void* p,uint64_t at,void* data,uint32_t n)->int{auto& d=*static_cast<Disk*>(p);++d.reads;
            if(d.failRead==int(d.reads))return -5;
            if(d.shortRead==int(d.reads))return int(n-1);
            if(at==1024 && n==1024)std::memcpy(data,d.sb,n);
            else if(at==4096 && n==32)std::memcpy(data,d.gdt,n);else return -22;return int(n);},
        [](void* p,uint64_t at,const void* data,uint32_t n)->int{auto& d=*static_cast<Disk*>(p);++d.writes;
            if(d.failWrite==int(d.writes))return -5;if(at!=1082 || n!=2)return -22;
            if(d.shortWrite==int(d.writes)){std::memcpy(d.sb+58,data,1);return 1;}
            if(!d.ignoreWrite)std::memcpy(d.sb+58,data,n);return int(n);},
        [](void* p)->int{auto& d=*static_cast<Disk*>(p);++d.flushes;return d.failFlush==int(d.flushes)?-5:0;}};}
};
int main(){
    constexpr uint64_t bytes=4096ull*4096;uint8_t scratch[32]{};
    Disk d;LargeDataSession s;
    assert(s.Begin(d.Io(),bytes,d.uuid,scratch,sizeof(scratch))==0 && s.dirty && d.writes==1 && d.flushes==1);
    assert(s.Finish(d.Io(),true)==0 && !LargeExt2Read16(d.sb+58));
    assert(s.Arm() && s.Finish(d.Io(),false)==-1 && !LargeExt2Read16(d.sb+58));
    assert(s.Finish(d.Io(),true)==0 && LargeExt2Read16(d.sb+58)==1 && s.clean);
    assert(s.Finish(d.Io(),true)==0 && d.writes==2);
    for(int fault=1;fault<=5;++fault){
        Disk bad;LargeDataSession session;
        if(fault<=2)bad.failRead=fault;else if(fault==3)bad.failWrite=1;else if(fault==4)bad.failFlush=1;else bad.failRead=3;
        assert(session.Begin(bad.Io(),bytes,bad.uuid,scratch,sizeof(scratch))<0 && !session.Arm());
        if(fault>=3)assert(session.dirtyAttempted && session.Begin(bad.Io(),bytes,bad.uuid,scratch,sizeof(scratch))<0);
        const auto writes=bad.writes;assert(session.Finish(bad.Io(),true)==0 && bad.writes==writes);
    }
    Disk dirty;put16(dirty.sb+58,0);LargeDataSession rejected;
    assert(rejected.Begin(dirty.Io(),bytes,dirty.uuid,scratch,sizeof(scratch))==-117 && dirty.reads==1 && !dirty.writes);
    Disk bad;put32(bad.gdt,1);LargeDataSession metadata;
    assert(metadata.Begin(bad.Io(),bytes,bad.uuid,scratch,sizeof(scratch))==-22 && !bad.writes);
    for(int shortAt=1;shortAt<=3;++shortAt){
        Disk x;LargeDataSession session;x.shortRead=shortAt;
        assert(session.Begin(x.Io(),bytes,x.uuid,scratch,sizeof(scratch))<0 && !session.Arm());
        assert(x.writes==(shortAt==3?1u:0u));
    }
    {Disk x;LargeDataSession session;x.shortWrite=1;
        assert(session.Begin(x.Io(),bytes,x.uuid,scratch,sizeof(scratch))<0 && session.dirtyAttempted && !session.Arm());
        assert(session.Begin(x.Io(),bytes,x.uuid,scratch,sizeof(scratch))<0 && x.writes==1);}
    {Disk x;LargeDataSession session;x.ignoreWrite=true;
        assert(session.Begin(x.Io(),bytes,x.uuid,scratch,sizeof(scratch))==-5 && session.dirtyAttempted && !session.Arm());
        assert(session.Begin(x.Io(),bytes,x.uuid,scratch,sizeof(scratch))<0 && x.writes==1);}
    for(int fault=1;fault<=2;++fault){
        Disk x;LargeDataSession session;assert(!session.Begin(x.Io(),bytes,x.uuid,scratch,sizeof(scratch)) && session.Arm());
        if(fault==1)x.failWrite=2;else x.failFlush=2;
        assert(session.Finish(x.Io(),true)<0 && !session.clean && session.cleanAttempted);
        const auto writes=x.writes;assert(session.Finish(x.Io(),true)<0 && x.writes==writes);
    }
    {Disk x;LargeDataSession session;assert(!session.Begin(x.Io(),bytes,x.uuid,scratch,sizeof(scratch)) && session.Arm());
        x.shortWrite=2;assert(session.Finish(x.Io(),true)<0 && !session.clean && session.cleanAttempted);
        assert(session.Finish(x.Io(),true)<0 && x.writes==2);}
}
