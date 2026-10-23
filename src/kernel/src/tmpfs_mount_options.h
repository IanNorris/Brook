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
}
