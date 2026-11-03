#include "guard_fat_profile.h"
#include "device.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace brook;
namespace {
std::vector<uint8_t> image;
bool shortRead=false;
std::FILE* diskFile=nullptr;uint64_t fileBytes=0;
void p16(uint8_t* p,uint16_t v) {p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
void p32(uint8_t* p,uint32_t v) {p16(p,uint16_t(v));p16(p+2,uint16_t(v>>16));}
int read(Device*,uint64_t offset,void* out,uint64_t n) {
    if(diskFile) {
        if(offset>fileBytes || n>fileBytes-offset || std::fseek(diskFile,long(offset),SEEK_SET))return -5;
        return int(std::fread(out,1,size_t(n),diskFile));
    }
    if(offset>image.size() || n>image.size()-offset)return -5;
    if(shortRead && offset)return int(n)-1;
    std::memcpy(out,image.data()+offset,n);return int(n);
}
int write(Device*,uint64_t,const void*,uint64_t) {assert(false);return -1;}
uint64_t blocks(Device*) {return (diskFile?fileBytes:image.size())/512;}
uint32_t blockSize(Device*) {return 512;}
void fat16(uint32_t clusters=8095,uint32_t fat=32) {
    const uint32_t total=1+fat*2+32+clusters;
    image.assign(uint64_t(total)*512,0);auto* b=image.data();b[0]=0xeb;
    p16(b+11,512);b[13]=1;p16(b+14,1);b[16]=2;p16(b+17,512);b[21]=0xf8;
    if(total<65536)p16(b+19,uint16_t(total));else p32(b+32,total);
    p16(b+22,fat);b[510]=0x55;b[511]=0xaa;
}
void fat32() {
    const uint32_t total=32+550*2+70000;
    image.assign(uint64_t(total)*512,0);auto* b=image.data();b[0]=0xeb;
    p16(b+11,512);b[13]=1;p16(b+14,32);b[16]=2;b[21]=0xf8;p32(b+32,total);
    p32(b+36,550);p32(b+44,2);p16(b+48,1);p16(b+50,6);b[510]=0x55;b[511]=0xaa;
    std::memcpy(b+82,"FAT32   ",8);
    std::memcpy(b+6*512,b,512);
    p32(b+512,0x41615252);p32(b+512+484,0x61417272);
    p32(b+512+488,0xffffffff);p32(b+512+492,0xffffffff);p32(b+512+508,0xaa550000);
}
}
int main(int argc,char** argv) {
    BlockDeviceOps ops{read,write,nullptr,nullptr,blocks,blockSize};
    Device dev{reinterpret_cast<const DeviceOps*>(&ops),"guard",DeviceType::Block,nullptr};
    GuardFatProfile p{};
    if(argc==2) {
        diskFile=std::fopen(argv[1],"rb");assert(diskFile);
        assert(!std::fseek(diskFile,0,SEEK_END));const long bytes=std::ftell(diskFile);
        assert(bytes>0 && !(bytes%512));fileBytes=uint64_t(bytes);
        assert(GuardFatCheckDevice(&dev,p));std::fclose(diskFile);return 0;
    }
    assert(argc==1);
    fat16();assert(GuardFatCheckDevice(&dev,p) && p.type==16 && p.clusters==8095);
    image[(1+32)*512+8]^=1;assert(!GuardFatCheckDevice(&dev,p) && !p.total && !p.clusters);
    fat16();shortRead=true;assert(!GuardFatCheckDevice(&dev,p));shortRead=false;
    fat16(4085);assert(!GuardFatCheckDevice(&dev,p));
    fat16(4086);assert(GuardFatCheckDevice(&dev,p));
    fat16(65524,256);assert(GuardFatCheckDevice(&dev,p));
    fat16(65525,256);assert(!GuardFatCheckDevice(&dev,p));
    fat16();image[16]=1;assert(!GuardFatCheckDevice(&dev,p));
    fat16();p16(image.data()+11,1024);assert(!GuardFatCheckDevice(&dev,p));
    fat16();p16(image.data()+19,uint16_t(blocks(&dev)+1));assert(!GuardFatCheckDevice(&dev,p));
    fat16();p32(image.data()+32,uint32_t(blocks(&dev)));assert(!GuardFatCheckDevice(&dev,p));
    fat16();p16(image.data()+19,0);p32(image.data()+32,uint32_t(blocks(&dev)));assert(!GuardFatCheckDevice(&dev,p));
    fat32();assert(GuardFatCheckDevice(&dev,p) && p.type==32 && p.clusters==70000);
    image[6*512+100]^=1;assert(!GuardFatCheckDevice(&dev,p));
    fat32();p16(image.data()+40,0x80);assert(!GuardFatCheckDevice(&dev,p));
    fat32();image[82]='X';assert(!GuardFatCheckDevice(&dev,p));
    fat32();p16(image.data()+50,32);assert(!GuardFatCheckDevice(&dev,p));
    fat32();image[512+484]^=1;assert(!GuardFatCheckDevice(&dev,p));
    fat32();p32(image.data()+512+488,70000);p32(image.data()+512+492,70001);assert(GuardFatCheckDevice(&dev,p));
    fat32();p32(image.data()+512+488,70001);assert(!GuardFatCheckDevice(&dev,p));
    fat32();p32(image.data()+512+492,70002);assert(!GuardFatCheckDevice(&dev,p));
    fat32();image[(32+550)*512+1]^=1;assert(!GuardFatCheckDevice(&dev,p));
}
