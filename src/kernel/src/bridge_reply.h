#pragma once
#include <stdint.h>

namespace brook {
// Stream a reply in fixed-size chunks, without allocation or printf. Explicit
// send preserves the existing caller contract; an automatic flush resets the
// buffer and continues with the next byte, never silently truncating a field.
template<void (*Send)(const char*)> struct BridgeReplyBuffer {
    char data[768] = {}; uint32_t size = 0;
    void append(char c){
        if(size+1==sizeof(data)){send();size=0;}
        data[size++]=c;
    }
    void text(const char* s){while(*s)append(*s++);}
    void number(uint64_t v,bool hex=false){
        char digits[32];uint32_t n=0;
        do{uint32_t d=v%(hex?16:10);digits[n++]=d<10?'0'+d:'a'+d-10;v/=hex?16:10;}while(v);
        while(n)append(digits[--n]);
    }
    void field(const char* name,uint64_t v){text("\"");text(name);text("\":");number(v);}
    void quoted(const char* s){
        append('"');
        const char* hex="0123456789abcdef";
        while(*s){const auto c=static_cast<unsigned char>(*s++);
            if(c=='"'||c=='\\'){append('\\');append(c);}
            else if(c<32){text("\\u00");append(hex[c>>4]);append(hex[c&15]);}
            else append(c);
        }
        append('"');
    }
    void send(){data[size]=0;Send(data);}
};
}
