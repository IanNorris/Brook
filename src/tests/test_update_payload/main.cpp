#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../shared/update/payload.h"
static std::string hash(const std::string& text) {
    uint8_t result[32];BrookSha256 s;BrookShaInit(&s);
    // Deliberately exercise incremental boundaries, not only the one-shot helper.
    for(unsigned char c:text)BrookShaAdd(&s,&c,1);
    BrookShaFinish(&s,result);char hex[65];
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",result[i]);return hex;
}
int main(){
    assert(hash("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(hash("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(hash(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    assert(BrookUpdateCrc((const uint8_t*)"123456789",9)==0xcbf43926);
    uint8_t a[128]{},b[128]{};assert(BrookUpdateNewest(a,b)==-1);
    memcpy(a,"BROOKUP1",8);BrookUpdatePut64(a+8,1);BrookUpdatePut64(a+24,64);BrookUpdatePut64(a+32,512);
    auto seal=[](uint8_t* p){BrookUpdatePut32(p+124,BrookUpdateCrc(p,124));};seal(a);
    assert(BrookUpdateRecordValid(a));assert(BrookUpdateNewest(a,b)==0);
    memcpy(b,a,128);assert(BrookUpdateNewest(a,b)==0);
    BrookUpdatePut32(b+16,1);seal(b);assert(BrookUpdateNewest(a,b)==-2);
    BrookUpdatePut64(b+8,2);seal(b);assert(BrookUpdateNewest(a,b)==1);
    // Any single-byte mutation must fail validation, including hashes/reserved/CRC.
    for(unsigned i=0;i<128;i++){a[i]^=1;assert(!BrookUpdateRecordValid(a));a[i]^=1;}
    BrookUpdatePut64(b+24,BROOK_UPDATE_KERNEL_MAX+1ull);seal(b);assert(!BrookUpdateRecordValid(b));
    BrookUpdatePut64(b+24,64);BrookUpdatePut64(b+32,BROOK_UPDATE_INITRD_MAX+1ull);seal(b);assert(!BrookUpdateRecordValid(b));
    puts("UPDATE_PAYLOAD_PASS");
}
