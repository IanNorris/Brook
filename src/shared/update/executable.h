#pragma once
#include "payload.h"
/* Identical eligibility in firmware and updater: a hashed but malformed newer
 * ELF must not make the updater overwrite the older slot firmware booted. */
static inline int BrookUpdateExecutable(const uint8_t* data,BrookUpdateSize size) {
    if(size<64||data[0]!=0x7f||data[1]!='E'||data[2]!='L'||data[3]!='F'||
       data[4]!=2||data[5]!=1||data[6]!=1||data[18]!=62||data[19]!=0||
       BrookUpdateGet32(data+20)!=1||data[17]!=0||(data[16]!=2&&data[16]!=3)||
       data[52]!=64||data[53]!=0)return 0;
    uint64_t offset=BrookUpdateGet64(data+32),entry=BrookUpdateGet64(data+24);
    unsigned stride=(unsigned)data[54]|(unsigned)data[55]<<8;
    unsigned count=(unsigned)data[56]|(unsigned)data[57]<<8;
    if(!count||stride<56||offset>size||count>(size-offset)/stride)return 0;
    uint64_t low=~(uint64_t)0,high=0;int executable=0;
    for(unsigned i=0;i<count;i++) {
        const uint8_t* p=data+offset+i*stride;
        uint64_t vaddr=BrookUpdateGet64(p+16),mem=BrookUpdateGet64(p+40);
        if(BrookUpdateGet32(p)!=1||!mem)continue;
        uint64_t file=BrookUpdateGet64(p+32),start=BrookUpdateGet64(p+8);
        if(file>mem||start>size||file>size-start||vaddr>~(uint64_t)0-mem)return 0;
        if(vaddr<low)low=vaddr;
        if(vaddr+mem>high)high=vaddr+mem;
        if((BrookUpdateGet32(p+4)&1)&&entry>=vaddr&&entry-vaddr<mem)executable=1;
    }
    return executable&&low==0xffffffff80000000ull&&high>low&&high-low<=256u*1024u*1024u;
}
