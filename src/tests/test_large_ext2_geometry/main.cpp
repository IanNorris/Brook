#include <cassert>
#include <cstdint>
#include <fstream>
#include <vector>
#include "large_ext2_geometry.h"
using namespace brook;
static uint16_t get16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1])<<8); }
static uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) | (uint32_t(get16(p+2))<<16); }
static void put32(uint8_t* p, uint32_t v) { for (int i=0;i<4;++i) p[i]=uint8_t(v>>(i*8)); }

int main(int argc, char** argv) {
    LargeExt2Geometry g{};
    assert(!LargeExt2GeometryMake(0,g));
    assert(LargeExt2GeometryMake(128u*32768,g));
    assert(g.groups==128 && g.descriptorBytes==4096 && g.descriptorBlocks==1);
    uint64_t first=0,end=0;
    assert(LargeExt2GroupBounds(g,0,first,end) && first==2 && end==32768);
    assert(LargeExt2GeometryMake(128u*32768+1,g));
    assert(g.groups==129 && g.descriptorBytes==4128 && g.descriptorBlocks==2);
    uint8_t descriptor[32]{};
    put32(descriptor,3); put32(descriptor+4,4); put32(descriptor+8,5);
    assert(LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    put32(descriptor,2); // second block of the two-block backup GDT
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    put32(descriptor,3); put32(descriptor+4,3);
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    put32(descriptor+4,4); put32(descriptor+8,32760);
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    put32(descriptor+8,32736); // 32-block inode table ends exactly at group end
    assert(LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    put32(descriptor+8,32737);
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    put32(descriptor+8,3);
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    put32(descriptor+8,4);
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,nullptr,sizeof(descriptor)));
    put32(descriptor+8,5); descriptor[14]=1; descriptor[15]=4; // 1025 free inodes
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,sizeof(descriptor)));
    descriptor[14]=descriptor[15]=0;
    assert(!LargeExt2GroupDescriptorValid(g,0,1024,descriptor,31));
    assert(!LargeExt2GroupDescriptorValid(g,0,1025,descriptor,sizeof(descriptor)));
    assert(LargeExt2GroupBounds(g,0,first,end) && first==3);
    assert(LargeExt2GroupBounds(g,1,first,end) && first==32768+3);
    assert(LargeExt2GroupBounds(g,2,first,end) && first==2u*32768);
    assert(LargeExt2GroupBounds(g,3,first,end) && first==3u*32768+3);
    assert(!LargeExt2GroupBounds(g,129,first,end));
    assert(LargeExt2HasBackup(1) && LargeExt2HasBackup(9) &&
           LargeExt2HasBackup(25) && LargeExt2HasBackup(49));
    assert(!LargeExt2HasBackup(6) && !LargeExt2HasBackup(15));
    assert(LargeExt2GeometryMake(125u*32768+2,g));
    assert(!LargeExt2GroupBounds(g,125,first,end)); // backup needs two blocks
    assert(LargeExt2GeometryMake(126u*32768+1,g));
    assert(LargeExt2GroupBounds(g,126,first,end) && first+1==end);
    assert(LargeExt2GeometryMake(128u*32768+3,g));
    uint8_t partial[32]{};
    put32(partial,128u*32768); put32(partial+4,128u*32768+1);
    put32(partial+8,128u*32768+2);
    assert(LargeExt2GroupDescriptorValid(g,128,32,partial,sizeof(partial)));
    assert(LargeExt2GeometryMake(128u*32768+2,g));
    assert(!LargeExt2GroupDescriptorValid(g,128,32,partial,sizeof(partial)));
    assert(LargeExt2GeometryMake(4096u*32768,g) && g.descriptorBlocks==32);
    LargeExt2Geometry forged=g; forged.descriptorBlocks=1;
    assert(!LargeExt2GroupBounds(forged,0,first,end));
    assert(!LargeExt2GeometryMake(4096u*32768+1,g));
    assert(!LargeExt2GeometryMake(UINT32_MAX,g));
    if (argc == 2) {
        std::ifstream image(argv[1], std::ios::binary);
        uint8_t sb[1024]{};
        image.seekg(1024); assert(image.read(reinterpret_cast<char*>(sb),sizeof(sb)));
        assert(get16(sb+56)==0xef53 && get32(sb+24)==2 && get32(sb+20)==0 && get16(sb+88)==128);
        assert(get32(sb+32)==32768 && get32(sb+92)==0 && get32(sb+96)==2);
        assert(get32(sb+100)==3 && get16(sb+206)==0);
        assert(LargeExt2GeometryMake(get32(sb+4),g) && g.descriptorBlocks>1);
        std::vector<uint8_t> table(g.descriptorBytes);
        image.seekg(4096); assert(image.read(reinterpret_cast<char*>(table.data()),table.size()));
        for (uint32_t group=0;group<g.groups;++group) {
            assert(LargeExt2GroupBounds(g,group,first,end));
            const uint8_t* d=table.data()+group*32;
            assert(LargeExt2GroupDescriptorValid(g,group,get32(sb+40),d,32));
            assert(get32(d)>=first && get32(d)<end);
            assert(get32(d+4)>=first && get32(d+4)<end);
            assert(get32(d+8)>=first && get32(d+8)<end);
        }
    }
    return 0;
}
