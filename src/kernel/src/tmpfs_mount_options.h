#pragma once
#include <stdint.h>
#include "tmpfs.h"
namespace brook
{
// Bounded tmpfs option subset. Validate the whole string before allocating or
// publishing a filesystem. Other Linux options still fail explicitly.
struct TmpfsMountOptions { uint16_t mode=01777; uint64_t bytes=TmpfsDefaultBytes; };
inline int ParseTmpfsMountOptions(const char* text,TmpfsMountOptions* out)
{
    if(!out)return -22;
    TmpfsMountOptions options{};
    if(text && *text) {
        const char* p=text;
        for(;;) {
            const bool mode=p[0]=='m' && p[1]=='o' && p[2]=='d' && p[3]=='e' && p[4]=='=';
            const bool size=p[0]=='s' && p[1]=='i' && p[2]=='z' && p[3]=='e' && p[4]=='=';
            if(!mode && !size)return -22;
            p+=5;
            uint64_t value=0;unsigned digits=0;
            const unsigned radix=mode?8:10;
            const uint64_t limit=mode?07777:TmpfsMaximumBytes();
            while(*p>='0' && *p<='9') {
                const unsigned digit=unsigned(*p-'0');
                if(digit>=radix || value>(limit-digit)/radix)return -22;
                value=value*radix+digit;++p;++digits;
            }
            if(!digits)return -22;
            if(size) {
                unsigned shift=0;
                if(*p=='k' || *p=='K'){shift=10;++p;}
                else if(*p=='m' || *p=='M'){shift=20;++p;}
                else if(*p=='g' || *p=='G'){shift=30;++p;}
                if(!value || value>(TmpfsMaximumBytes()>>shift))return -22;
                value<<=shift;
                options.bytes=(value+4095)&~uint64_t{4095};
            } else options.mode=uint16_t(value);
            if(*p && *p!=',')return -22;
            if(!*p)break;
            if(!*++p)return -22;
        }
    }
    *out=options;return 0;
}

// Each devpts mount is an independent instance. Accept the mode fields used
// by Linux container launchers and refuse an implicit global PTY instance.
inline int ParseDevptsOptions(const char* data,uint16_t* ptmxMode)
{
    if(!data || !ptmxMode)return -22;
    bool fresh=false,haveMaster=false,haveSlave=false;
    uint16_t master=0000;
    const char* p=data;
    while(*p) {
        const char* end=p;while(*end && *end!=',')++end;
        constexpr char instance[]="newinstance",masterKey[]="ptmxmode=",slaveKey[]="mode=";
        auto match=[&](const char* key,unsigned count) {
            if(static_cast<unsigned>(end-p)<count)return false;
            for(unsigned i=0;i<count;++i)if(p[i]!=key[i])return false;
            return true;
        };
        if(static_cast<unsigned>(end-p)==sizeof(instance)-1 && match(instance,sizeof(instance)-1)) {
            if(fresh)return -22;
            fresh=true;
        } else {
            const bool isMaster=match(masterKey,sizeof(masterKey)-1);
            if(!isMaster && !match(slaveKey,sizeof(slaveKey)-1))return -22;
            bool& seen=isMaster ? haveMaster : haveSlave;
            if(seen)return -22;
            seen=true;
            auto* value=p+(isMaster ? sizeof(masterKey) : sizeof(slaveKey))-1;
            if(value==end)return -22;
            unsigned mode=0;
            for(;value!=end;++value) {
                if(*value<'0' || *value>'7')return -22;
                mode=(mode<<3)+unsigned(*value-'0');
                if(mode>0777)return -22;
            }
            if(isMaster)master=static_cast<uint16_t>(mode);
        }
        p=end;
        if(*p && !*++p)return -22;
    }
    if(!fresh)return -22;
    *ptmxMode=master;return 0;
}

}
