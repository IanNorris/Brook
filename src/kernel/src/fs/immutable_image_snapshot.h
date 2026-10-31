#pragma once
#include <stdint.h>
#include "../../../shared/update/payload.h"
namespace brook {
// First hardware profile: one boot-lifetime snapshot, with 256 MiB left for
// indexes, normal kernel work and maintenance recovery before applications.
constexpr uint64_t AppSnapshotLimit=512ull*1024*1024;
constexpr uint64_t AppSnapshotHeadroom=256ull*1024*1024;
struct AppSnapshotPlan {char source[64]{};uint64_t bytes=0;uint8_t hash[32]{};};
inline bool AppSnapshotParse(const char* text,uint32_t length,AppSnapshotPlan& out) {
    AppSnapshotPlan result{};
    if(!text||!length||length>256)return false;
    if(text[length-1]=='\n')--length;
    uint32_t end=0;while(end<length&&text[end]!=' ')end++;
    if(!end||end>=sizeof(result.source)||end==length)return false;
    for(uint32_t i=0;i<end;i++)result.source[i]=text[i];
    auto equal=[](const char* a,const char* b){while(*a&&*a==*b){a++;b++;}return *a==*b;};
    if(!equal(result.source,"/media/usb/APPS.BRK")&&!equal(result.source,"/media/usb/UPDATES/A/APPS.BRK")&&
       !equal(result.source,"/media/usb/UPDATES/B/APPS.BRK")&&
       !equal(result.source,"/media/system/APPS.BRK"))return false;
    uint32_t cursor=end+1;
    if(length-cursor<6||text[cursor]!='/'||text[cursor+1]!='n'||text[cursor+2]!='i'||text[cursor+3]!='x'||text[cursor+4]!=' ')return false;
    cursor+=5;uint32_t digits=0;
    while(cursor<length&&text[cursor]!=' ') {
        unsigned n=unsigned(text[cursor++]-'0');
        if(n>9||result.bytes>(AppSnapshotLimit-n)/10)return false;
        result.bytes=result.bytes*10+n;digits++;
    }
    if(!digits||result.bytes<4096||(result.bytes&4095)||result.bytes>AppSnapshotLimit||cursor==length)return false;
    cursor++;if(length-cursor!=64)return false;
    for(unsigned i=0;i<64;i++) {
        unsigned char c=text[cursor+i];unsigned n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;
        if(n==16)return false;
        result.hash[i/2]|=uint8_t(n<<(i%2?0:4));
    }
    out=result;return true;
}
enum class AppSnapshotError {None,Bounds,Memory,Read,Hash};
// Io owns no published device: failure returns its complete allocation exactly
// once. A successful caller owns the returned memory and must keep it immutable.
// Io: allocate(bytes), release(pointer,bytes), read(offset,destination,bytes).
template<class Io> void* AppSnapshotCopy(Io& io,const AppSnapshotPlan& plan,uint64_t freeBytes,AppSnapshotError& error) {
    error=AppSnapshotError::Bounds;
    if(plan.bytes<4096||(plan.bytes&4095)||plan.bytes>AppSnapshotLimit)return nullptr;
    error=AppSnapshotError::Memory;
    if(freeBytes<AppSnapshotHeadroom||plan.bytes>freeBytes-AppSnapshotHeadroom)return nullptr;
    auto* memory=static_cast<uint8_t*>(io.allocate(plan.bytes));if(!memory)return nullptr;
    BrookSha256 hash;BrookShaInit(&hash);uint64_t position=0;
    while(position<plan.bytes) {
        uint32_t request=plan.bytes-position>65536?65536:uint32_t(plan.bytes-position);
        int count=io.read(position,memory+position,request);
        if(count<=0||uint32_t(count)>request){io.release(memory,plan.bytes);error=AppSnapshotError::Read;return nullptr;}
        BrookShaAdd(&hash,memory+position,uint32_t(count));position+=uint32_t(count);
    }
    uint8_t actual[32];BrookShaFinish(&hash,actual);
    if(!BrookUpdateEqual(actual,plan.hash,32)){io.release(memory,plan.bytes);error=AppSnapshotError::Hash;return nullptr;}
    error=AppSnapshotError::None;return memory;
}
}
