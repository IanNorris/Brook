#pragma once
#include <stdint.h>

namespace brook {
struct WritableDesktopPlan { uint64_t bytes=0;uint8_t uuid[16]{}; };
inline uint16_t StoreGet16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline uint32_t StoreGet32(const uint8_t* p){return uint32_t(StoreGet16(p))|(uint32_t(StoreGet16(p+2))<<16);}
inline bool WritableDesktopPlanParse(const char* record,uint32_t bytes,WritableDesktopPlan& out) {
    constexpr char prefix[]="BRKSTORE1 ";
    if(!record || bytes<sizeof(prefix)+38 || bytes>96)return false;
    if(record[bytes-1]=='\n')--bytes;
    uint32_t at=0;for(;at<sizeof(prefix)-1;++at)if(record[at]!=prefix[at])return false;
    WritableDesktopPlan plan;uint32_t digits=0;
    while(at<bytes && record[at]>='0' && record[at]<='9'){
        const uint32_t n=record[at++]-'0';if(plan.bytes>(0xfffffe00ULL-n)/10)return false;
        plan.bytes=plan.bytes*10+n;++digits;
    }
    if(!digits || at>=bytes || record[at++]!=' ' || bytes-at!=36 ||
       plan.bytes<16*1024*1024 || (plan.bytes&4095))return false;
    unsigned nibble=0;uint8_t any=0;
    for(unsigned i=0;i<36;++i){
        const char c=record[at+i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return false;continue;}
        unsigned value;
        if(c>='0'&&c<='9')value=c-'0';else if(c>='a'&&c<='f')value=c-'a'+10;else return false;
        plan.uuid[nibble/2]=uint8_t((plan.uuid[nibble/2]<<4)|value);any|=value;++nibble;
    }
    if(!any)return false;
    out=plan;return true;
}
inline bool WritableDesktopPlanSame(const WritableDesktopPlan& a,const WritableDesktopPlan& b){
    if(a.bytes!=b.bytes)return false;
    for(unsigned i=0;i<16;++i)if(a.uuid[i]!=b.uuid[i])return false;return true;
}
// Exact builder geometry/features, not a general ext2 verifier. This bounds
// every allocation and metadata range used during mount before trusting it.
inline bool WritableDesktopHeaderValid(const uint8_t sb[1024],const WritableDesktopPlan& plan) {
    if(StoreGet16(sb+56)!=0xef53 || StoreGet16(sb+58)!=1 || StoreGet32(sb+72)!=0 ||
       StoreGet32(sb+76)!=1 || StoreGet16(sb+88)!=128 || StoreGet32(sb+20)!=0 ||
       StoreGet32(sb+24)!=2 || StoreGet32(sb+28)!=2 || StoreGet32(sb+32)!=32768 ||
       StoreGet32(sb+36)!=32768 || StoreGet32(sb+92)!=0 || StoreGet32(sb+96)!=2 ||
       StoreGet32(sb+100)!=3 || StoreGet32(sb+84)!=11 || StoreGet16(sb+90)!=0)return false;
    for(unsigned i=0;i<16;++i)if(sb[104+i]!=plan.uuid[i])return false;
    const uint32_t blocks=StoreGet32(sb+4),inodes=StoreGet32(sb),ipg=StoreGet32(sb+40);
    if(uint64_t(blocks)*4096!=plan.bytes || StoreGet32(sb+8)!=0 || StoreGet32(sb+12)>blocks ||
       StoreGet32(sb+16)>inodes || !ipg || ipg>32768 || ipg%32)return false;
    const uint32_t groups=(blocks+32767)/32768;
    return groups && groups<=32 && inodes>=11 && uint64_t(ipg)*groups==inodes;
}
inline bool WritableDesktopGroupsValid(const uint8_t sb[1024],const uint8_t* gdt,uint32_t bytes) {
    const uint32_t blocks=StoreGet32(sb+4),groups=(blocks+32767)/32768;
    if(!groups || groups>32 || bytes!=groups*32)return false;
    const uint32_t tableBlocks=(StoreGet32(sb+40)*128+4095)/4096;
    for(uint32_t i=0;i<groups;++i){
        const auto* d=gdt+i*32;const uint32_t blockMap=StoreGet32(d),inodeMap=StoreGet32(d+4),table=StoreGet32(d+8);
        const uint64_t begin=uint64_t(i)*32768,end=(begin+32768<blocks?begin+32768:blocks);
        bool backup=i<2;
        const uint32_t primes[]={3,5,7};
        for(uint32_t prime:primes){uint32_t n=i;while(n>1 && n%prime==0)n/=prime;if(n==1)backup=true;}
        const uint64_t first=begin+(backup?2:0);
        if(blockMap<first || blockMap>=end || inodeMap<first || inodeMap>=end || blockMap==inodeMap ||
           table<first || table>=end || tableBlocks>end-table ||
           (blockMap>=table && blockMap<uint64_t(table)+tableBlocks) ||
           (inodeMap>=table && inodeMap<uint64_t(table)+tableBlocks))return false;
        if(StoreGet16(d+12)>end-begin || StoreGet16(d+14)>StoreGet32(sb+40) || StoreGet16(d+16)>StoreGet32(sb+40))return false;
    }
    return true;
}
struct WritableDesktopIo {
    void* context;
    int (*read)(void*,uint64_t,void*,uint32_t);
    int (*write)(void*,uint64_t,const void*,uint32_t);
    int (*flush)(void*);
};
struct WritableDesktopSession {
    bool dirty=false,armed=false,clean=false,dirtyAttempted=false,cleanAttempted=false;
    int Begin(const WritableDesktopIo& io,const WritableDesktopPlan& plan) {
        if(dirty || armed || !io.read || !io.write || !io.flush)return -22;
        uint8_t sb[1024]{},gdt[1024]{};
        int r=io.read(io.context,1024,sb,sizeof(sb));if(r!=int(sizeof(sb)))return r<0?r:-5;
        if(!WritableDesktopHeaderValid(sb,plan))return -22;
        const uint32_t bytes=((StoreGet32(sb+4)+32767)/32768)*32;
        r=io.read(io.context,4096,gdt,bytes);if(r!=int(bytes))return r<0?r:-5;
        if(!WritableDesktopGroupsValid(sb,gdt,bytes))return -22;
        const uint8_t state[2]={0,0};
        dirtyAttempted=true;
        r=io.write(io.context,1082,state,2);if(r!=2)return r<0?r:-5;
        r=io.flush(io.context);if(r)return r<0?r:-5;
        dirty=true;return 0;
    }
    bool Arm(){if(!dirty || clean)return false;armed=true;return true;}
    // Caller also proves successful global VFS sync. Failure never sets clean.
    int Finish(const WritableDesktopIo& io,bool finalSyncOwner) {
        if(!armed)return 0;
        if(!finalSyncOwner)return -1;
        if(clean)return 0;
        const uint8_t state[2]={1,0};cleanAttempted=true;const int r=io.write(io.context,1082,state,2);
        if(r!=2)return r<0?r:-5;
        const int flush=io.flush(io.context);if(flush)return flush<0?flush:-5;
        clean=true;return 0;
    }
};
}
