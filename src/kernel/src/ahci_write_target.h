#pragma once
#include "ata_identify.h"
#include <stdint.h>
#include <string.h>

namespace brook {
// Strict, bounded boot-time policy. ASCII record:
// BRKAHCI1\n<vendor>,<device>,<bus>,<slot>,<function>,<port>,<sectors>,
// <logical-bytes>,<physical-bytes>\n<model>\n<serial>\n
// Numeric fields are decimal; identity fields are exact printable ATA text.
struct AhciWriteTarget {
    uint64_t vendor=0,device=0,bus=0,slot=0,function=0,port=0,sectors=0,logical=0,physical=0;
    char model[41]{},serial[21]{};
};
inline bool AhciTargetNumber(const char*& p,const char* end,uint64_t& value,char delimiter) {
    if(p==end || *p<'0' || *p>'9')return false;
    value=0;const char* first=p;
    while(p<end && *p>='0' && *p<='9') {
        const unsigned digit=unsigned(*p++-'0');
        if(value>(~uint64_t{0}-digit)/10)return false;
        value=value*10+digit;
    }
    if(p-first>1 && *first=='0')return false;
    if(p==end || *p++!=delimiter)return false;
    return true;
}
inline bool AhciTargetText(const char*& p,const char* end,char* out,unsigned capacity) {
    unsigned n=0;
    while(p<end && *p!='\n') {
        const unsigned char c=static_cast<unsigned char>(*p++);
        if(c<32 || c>126 || c=='?' || n+1>=capacity)return false;
        out[n++]=char(c);
    }
    if(!n || out[0]==' ' || out[n-1]==' ' || p==end)return false;
    ++p;out[n]=0;return true;
}
inline bool AhciWriteTargetParse(const char* data,uint32_t length,AhciWriteTarget& target) {
    target={};if(!data || length<12 || length>192 || memcmp(data,"BRKAHCI1\n",9))return false;
    AhciWriteTarget parsed{};
    const char* p=data+9;const char* end=data+length;
    uint64_t* fields[]={&parsed.vendor,&parsed.device,&parsed.bus,&parsed.slot,&parsed.function,
                        &parsed.port,&parsed.sectors,&parsed.logical,&parsed.physical};
    for(unsigned i=0;i<9;++i)if(!AhciTargetNumber(p,end,*fields[i],i==8?'\n':','))return false;
    if(parsed.vendor>65535 || parsed.device>65535 || parsed.bus>255 || parsed.slot>31 ||
       parsed.function>7 || parsed.port>31 || !parsed.sectors || parsed.sectors>(1ull<<48) ||
       parsed.logical!=512 || parsed.physical<512 || parsed.physical>16777216 ||
       (parsed.physical&(parsed.physical-1)))return false;
    if(!AhciTargetText(p,end,parsed.model,sizeof(parsed.model)) ||
       !AhciTargetText(p,end,parsed.serial,sizeof(parsed.serial)) || p!=end)return false;
    target=parsed;return true;
}
inline bool AhciWriteTargetMatches(const AhciWriteTarget& t,uint16_t vendor,uint16_t device,
                                   uint8_t bus,uint8_t slot,uint8_t function,uint32_t port,
                                   const AtaIdentifyInfo& info,bool flushExt) {
    return flushExt && info.model[0] && info.serial[0] &&
        !strchr(info.model,'?') && !strchr(info.serial,'?') &&
        t.vendor==vendor && t.device==device && t.bus==bus && t.slot==slot &&
        t.function==function && t.port==port && t.sectors==info.sectors &&
        t.logical==info.logicalSectorBytes && t.physical==info.physicalSectorBytes &&
        !strcmp(t.model,info.model) && !strcmp(t.serial,info.serial);
}
}
