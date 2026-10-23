#pragma once
#include <stdint.h>
namespace brook
{
// Bounded tmpfs option subset. Validate the whole string before allocating or
// publishing a filesystem. Other Linux options still fail explicitly.
struct TmpfsMountOptions { uint16_t mode=01777; };
inline int ParseTmpfsMountOptions(const char* text,TmpfsMountOptions* out)
{
    if(!out)return -22;
    TmpfsMountOptions options{};
    if(text && *text) {
        const char* p=text;
        for(;;) {
            const char key[]="mode=";
            for(unsigned i=0;i<sizeof(key)-1;++i)if(*p++!=key[i])return -22;
            uint32_t value=0;unsigned digits=0;
            while(*p && *p!=',') {
                if(*p<'0' || *p>'7' || value>(07777u-uint32_t(*p-'0'))/8)return -22;
                value=value*8+uint32_t(*p++-'0');++digits;
            }
            if(!digits)return -22;
            options.mode=uint16_t(value);
            if(!*p)break;
            if(!*++p)return -22;
        }
    }
    *out=options;return 0;
}
}
