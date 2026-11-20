#pragma once

#include "core_dump.h"

namespace brook {

constexpr uint64_t CorePageSize=4096, CoreDumpLimit=1024ull*1024*1024;

struct CoreElfHeader {
    unsigned char ident[16]{0x7f,'E','L','F',2,1,1,3};
    uint16_t type=4,machine=62;
    uint32_t version=1;
    uint64_t entry=0,programOffset=64,sectionOffset=0;
    uint32_t flags=0;
    uint16_t size=64,programSize=56,programCount=0,sectionSize=0,sectionCount=0,sectionNames=0;
};
struct CoreProgramHeader {
    uint32_t type,flags;
    uint64_t offset,address,physical,fileSize,memorySize,align;
};
struct CoreSectionHeader {
    uint32_t name=0,type=0;
    uint64_t flags=0,address=0,offset=0,size=0;
    uint32_t link=0,info=0;
    uint64_t align=0,entrySize=0;
};
static_assert(sizeof(CoreSectionHeader)==64);
constexpr uint32_t CorePnXnum=0xffff;
constexpr uint64_t CoreHeaderBytes(uint32_t count) {
    return sizeof(CoreElfHeader)+uint64_t(count)*sizeof(CoreProgramHeader)+
           (count>=CorePnXnum?sizeof(CoreSectionHeader):0);
}
static_assert(sizeof(CoreElfHeader)==64 && sizeof(CoreProgramHeader)==56);
struct CoreNoteHeader {uint32_t nameSize,descSize,type;};
constexpr uint32_t CoreNtStatus=1,CoreNtInfo=3,CoreNtAuxv=6,CoreNtFile=0x46494c45;
constexpr uint64_t CoreNoteSize(uint64_t desc){return 20+((desc+3)&~uint64_t{3});}
constexpr uint64_t CorePageAlign(uint64_t bytes){return (bytes+CorePageSize-1)&~(CorePageSize-1);}

template<class Output>
bool CoreWriteNote(Output& output,uint32_t type,const void* desc,uint32_t bytes){
    const CoreNoteHeader header{5,bytes,type};
    const char owner[8]="CORE";
    const uint32_t zero=0;
    return output.write(&header,sizeof(header)) && output.write(owner,sizeof(owner)) &&
        output.write(desc,bytes) && output.write(&zero,(-bytes)&3u);
}

template<class Output>
bool CoreWriteHeader(Output& output,const CoreProgramHeader* programs,uint32_t count) {
    if(!count)return false;
    CoreElfHeader header;
    CoreSectionHeader section{};
    if(count>=CorePnXnum){
        header.programCount=CorePnXnum;
        header.sectionOffset=CoreHeaderBytes(count)-sizeof(section);
        header.sectionSize=sizeof(section);
        header.sectionCount=1;
        section.info=count;
    }else header.programCount=static_cast<uint16_t>(count);
    return output.write(&header,sizeof(header)) && output.write(programs,uint64_t(count)*sizeof(*programs)) &&
        (count<CorePnXnum || output.write(&section,sizeof(section)));
}
}
