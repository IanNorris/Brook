#pragma once
#include <stdint.h>

namespace brook {
inline constexpr uint32_t DebugJobMaxRequest=32768;
struct DebugJobRequest {
    uint32_t uid,argc,envc;
    const char* argv[64];
    const char* env[64];
    const char* cwd;
};
// BJ01, little-endian uid/argc/envc, then NUL-terminated argv, cwd, env.
// No implicit truncation, shell parsing, or pointers into an unbounded buffer.
inline bool DebugJobDecode(const uint8_t* bytes,uint32_t size,DebugJobRequest* out) {
    if (size<16 || size>DebugJobMaxRequest || bytes[0]!='B' || bytes[1]!='J' ||
        bytes[2]!='0' || bytes[3]!='1') return false;
    auto word=[&](uint32_t p) {
        return uint32_t(bytes[p]) | uint32_t(bytes[p+1])<<8 |
               uint32_t(bytes[p+2])<<16 | uint32_t(bytes[p+3])<<24;
    };
    out->uid=word(4);out->argc=word(8);out->envc=word(12);
    if ((out->uid!=0 && out->uid!=1000) || !out->argc || out->argc>64 || out->envc>64) return false;
    uint32_t pos=16;
    auto string=[&](const char** value) {
        if (pos>=size) return false;
        uint32_t start=pos;while (pos<size && bytes[pos]) ++pos;
        if (pos==size) return false;
        *value=reinterpret_cast<const char*>(bytes+start);++pos;return true;
    };
    for (uint32_t i=0;i<out->argc;++i) if (!string(&out->argv[i])) return false;
    if (out->argv[0][0]!='/') return false;
    if (!string(&out->cwd) || out->cwd[0]!='/') return false;
    for (uint32_t i=0;i<out->envc;++i) {
        if (!string(&out->env[i])) return false;
        const char* p=out->env[i];
        if (!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || *p=='_')) return false;
        while ((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
               (*p>='0' && *p<='9') || *p=='_') ++p;
        if (*p!='=') return false;
    }
    return pos==size;
}
}
