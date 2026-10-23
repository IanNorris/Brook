#pragma once
#include "mount_namespace.h"
namespace brook {
// Bounded Linux mountinfo serialization shared with host tests. Cached rows
// own all their bytes; this formatter has no VFS/backend/lock dependency.
inline int FormatMountInfo(const VfsMountInfo* rows,uint32_t count,char* out,uint32_t capacity,uint32_t* written) {
    if(!out || !capacity || !written || (count&&!rows))return -22;
    uint32_t used=0;int error=0;
    auto character=[&](char c) {if(used+1>=capacity)error=-75;else out[used++]=c;};
    auto literal=[&](const char* text) {for(;*text && !error;++text)character(*text);};
    auto number=[&](uint64_t value) {char digits[20];unsigned n=0;do {digits[n++]=char('0'+value%10);value/=10;}while(value);while(n&&!error)character(digits[--n]);};
    auto escape=[&](const char* text) {
        for(;*text && !error;++text) {
            unsigned char c=*text;
            if(c==' ' || c=='\t' || c=='\n' || c=='\\') {character('\\');character(char('0'+(c>>6)));character(char('0'+((c>>3)&7)));character(char('0'+(c&7)));}
            else character(char(c));
        }
    };
    for(uint32_t i=0;i<count&&!error;++i) {
        const auto& r=rows[i];
        if(!r.id || r.id>0x7ffffffeULL || r.parent>0x7ffffffeULL || (r.attributes&~uint64_t{7}) || r.root[0]!='/' || r.mountPoint[0]!='/') {error=-95;break;}
        number(r.id);character(' ');number(r.parent);character(' ');
        number(((r.device>>8)&0xfff)|((r.device>>32)&0xfffff000));character(':');
        number((r.device&0xff)|((r.device>>12)&0xffffff00));character(' ');
        escape(r.root);character(' ');escape(r.mountPoint);literal(r.attributes&1?" ro":" rw");
        if(r.attributes&2)literal(",nosuid");if(r.attributes&4)literal(",nodev");
        literal(" - ");escape(r.fsType);character(' ');escape(r.fsType);literal(" rw\n");
    }
    out[used]=0;*written=error?0:used;return error;
}
} // namespace brook
