#pragma once
/* Canonical little-endian update record shared by EFI, RAM updater and tests.
 * No packed native structures, heap allocation or unaligned integer accesses. */
#include <stdint.h>
typedef __SIZE_TYPE__ BrookUpdateSize;
#define BROOK_UPDATE_RECORD_BYTES 128u
#define BROOK_UPDATE_KERNEL_MAX (8u * 1024u * 1024u)
#define BROOK_UPDATE_INITRD_MAX (64u * 1024u * 1024u)
static inline uint32_t BrookUpdateGet32(const uint8_t* p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline uint64_t BrookUpdateGet64(const uint8_t* p) {
    return (uint64_t)BrookUpdateGet32(p)|((uint64_t)BrookUpdateGet32(p+4)<<32);
}
static inline void BrookUpdatePut32(uint8_t* p,uint32_t v) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
static inline void BrookUpdatePut64(uint8_t* p,uint64_t v) {
    for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i));
}
static inline uint32_t BrookUpdateCrc(const uint8_t* p,BrookUpdateSize n) {
    uint32_t v=0xffffffffu;
    for(BrookUpdateSize i=0;i<n;i++) { v^=p[i];for(unsigned j=0;j<8;j++)v=(v>>1)^(0xedb88320u & (0u-(v&1))); }
    return ~v;
}
static inline int BrookUpdateEqual(const uint8_t* a,const uint8_t* b,BrookUpdateSize n) {
    unsigned v=0;for(BrookUpdateSize i=0;i<n;i++)v|=a[i]^b[i];return v==0;
}
static inline uint32_t BrookShaRotate(uint32_t x,unsigned n) {return (x>>n)|(x<<(32-n));}
typedef struct {uint32_t h[8];uint64_t bytes;BrookUpdateSize used;uint8_t block[64];} BrookSha256;
static inline void BrookShaBlock(BrookSha256* s,const uint8_t* p) {
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for(unsigned i=0;i<16;i++)w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
    for(unsigned i=16;i<64;i++) {
        uint32_t a=w[i-15],b=w[i-2];
        w[i]=w[i-16]+(BrookShaRotate(a,7)^BrookShaRotate(a,18)^(a>>3))+w[i-7]+(BrookShaRotate(b,17)^BrookShaRotate(b,19)^(b>>10));
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for(unsigned i=0;i<64;i++) {
        uint32_t t=h+(BrookShaRotate(e,6)^BrookShaRotate(e,11)^BrookShaRotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(BrookShaRotate(a,2)^BrookShaRotate(a,13)^BrookShaRotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static inline void BrookShaInit(BrookSha256* s) {
    static const uint32_t initial[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for(unsigned i=0;i<8;i++)s->h[i]=initial[i];
    s->bytes=0;s->used=0;
}
static inline void BrookShaAdd(BrookSha256* s,const uint8_t* p,BrookUpdateSize n) {
    s->bytes+=n;
    while(n) {
        BrookUpdateSize take=64-s->used;if(take>n)take=n;
        for(BrookUpdateSize i=0;i<take;i++)s->block[s->used+i]=p[i];
        s->used+=take;p+=take;n-=take;
        if(s->used==64){BrookShaBlock(s,s->block);s->used=0;}
    }
}
static inline void BrookShaFinish(BrookSha256* s,uint8_t out[32]) {
    uint64_t bits=s->bytes*8;s->block[s->used++]=0x80;
    if(s->used>56){while(s->used<64)s->block[s->used++]=0;BrookShaBlock(s,s->block);s->used=0;}
    while(s->used<56)s->block[s->used++]=0;
    for(unsigned i=0;i<8;i++)s->block[63-i]=(uint8_t)(bits>>(i*8));
    BrookShaBlock(s,s->block);
    for(unsigned i=0;i<32;i++)out[i]=(uint8_t)(s->h[i/4]>>(24-8*(i%4)));
}
static inline void BrookUpdateHash(const uint8_t* p,BrookUpdateSize n,uint8_t out[32]) {
    BrookSha256 s;BrookShaInit(&s);BrookShaAdd(&s,p,n);BrookShaFinish(&s,out);
}
/* magic[8], generation u64, slot u32, flags u32 (zero), lengths u64[2],
 * SHA-256[2], reserved zero[20], CRC32[124]. generation zero is unselected. */
static inline int BrookUpdateRecordValid(const uint8_t* r) {
    static const uint8_t magic[8]={'B','R','O','O','K','U','P','1'};
    if(!BrookUpdateEqual(r,magic,8)||!BrookUpdateGet64(r+8)||BrookUpdateGet32(r+16)>1||BrookUpdateGet32(r+20))return 0;
    uint64_t k=BrookUpdateGet64(r+24),i=BrookUpdateGet64(r+32);
    if(k<64||k>BROOK_UPDATE_KERNEL_MAX||i<512||i>BROOK_UPDATE_INITRD_MAX)return 0;
    for(unsigned j=104;j<124;j++)if(r[j])return 0;
    return BrookUpdateGet32(r+124)==BrookUpdateCrc(r,124);
}
/* -1 means no usable record, -2 means conflicting equal generations. */
static inline int BrookUpdateNewest(const uint8_t* a,const uint8_t* b) {
    int av=BrookUpdateRecordValid(a),bv=BrookUpdateRecordValid(b);
    if(!av)return bv?1:-1;
    if(!bv)return 0;
    uint64_t ag=BrookUpdateGet64(a+8),bg=BrookUpdateGet64(b+8);
    if(ag==bg)return BrookUpdateEqual(a,b,128)?0:-2;
    return ag>bg?0:1;
}
