#pragma once
#include <stdint.h>

namespace brook {

struct PathWalkOptions {
    bool followFinal = true;
    bool allowMissingFinal = false;
    bool parentOnly = false;
    // An open with O_CREAT and a missing trailing-slash leaf reports EISDIR.
    int missingTrailingError = -2;
};

// Provider: Stat(path, Node&), Search(path, const Node&), Readlink(path,buf,size).
// Node supplies isDir/isSymlink. Caller must stabilize the namespace through
// the eventual operation; the walk alone is not an authorization capability.
// Bounded, iterative, allocation-free. Dotdot is consumed only after symlinks.
template<class Node, class Provider>
int WalkPath(const char* path, char* output, uint32_t capacity, Provider& fs,
             PathWalkOptions options = {})
{
    if (!output || capacity < 2) return -22;
    output[0] = 0;
    if (!path || !path[0]) return -2;
    auto length = [](const char* s) { uint32_t n=0; while (s[n]) ++n; return n; };
    auto copy = [](char* to, const char* from) { while ((*to++=*from++)) {} };
    char remaining[512], current[512] = "/";
    if (length(path) >= sizeof(remaining)) return -36;
    copy(remaining,path);
    Node directory{};
    if (int e=fs.Stat(current,directory)) return e;
    uint32_t links=0;
    for (;;) {
        const char* next=remaining;
        while (*next=='/') ++next;
        if (!*next) {
            if (length(current)>=capacity) return -36;
            copy(output,current); return 0;
        }
        if (!directory.isDir) return -20;
        if (int e=fs.Search(current,directory)) return e;
        char component[256]; uint32_t count=0;
        while (*next && *next!='/') {
            if (count+1>=sizeof(component)) return -36;
            component[count++]=*next++;
        }
        component[count]=0;
        char tail[512]; copy(tail,next);
        const char* suffix=tail; while (*suffix=='/') ++suffix;
        bool final=!*suffix;
        bool trailing=final && tail[0];
        uint32_t parentLength=length(current);
        uint32_t slash=parentLength>1?1:0;
        if (options.parentOnly && final) {
            if (parentLength+slash+count+(trailing?1:0)>=capacity) return -36;
            copy(output,current); uint32_t n=parentLength;
            if (slash) output[n++]='/';
            for (uint32_t i=0;i<count;++i) output[n++]=component[i];
            if (trailing) output[n++]='/';
            output[n]=0; return 0;
        }
        bool dot=component[0]=='.' && !component[1];
        bool dotdot=component[0]=='.' && component[1]=='.' && !component[2];
        if (dot || dotdot) {
            if (dotdot) {
                uint32_t n=parentLength;
                while (n>1 && current[n-1]!='/') --n;
                if (n>1) --n;
                current[n]=0;
                if (int e=fs.Stat(current,directory)) return e;
            }
            copy(remaining,tail); continue;
        }
        if (parentLength+slash+count>=sizeof(current)) return -36;
        if (slash) current[parentLength]='/';
        copy(current+parentLength+slash,component);
        Node child{};
        int error=fs.Stat(current,child);
        if (error) {
            if (error!=-2 || !final || !options.allowMissingFinal) return error;
            if (trailing && options.missingTrailingError) return options.missingTrailingError;
            if (length(current)>=capacity) return -36;
            copy(output,current); return 0;
        }
        if (child.isSymlink && (!final || options.followFinal || trailing)) {
            if (++links>40) return -40;
            char target[512];
            int bytes=fs.Readlink(current,target,sizeof(target)-1);
            if (bytes<0) return bytes;
            if (!bytes) return -2;
            if (bytes>=static_cast<int>(sizeof(target)-1)) return -36;
            target[bytes]=0;
            if (static_cast<uint32_t>(bytes)+length(tail)>=sizeof(remaining)) return -36;
            copy(remaining,target); copy(remaining+bytes,tail);
            if (target[0]=='/') {
                current[0]='/';current[1]=0;
                if (int e=fs.Stat(current,directory)) return e;
            } else current[parentLength]=0;
            continue;
        }
        if (tail[0] && !child.isDir) return -20;
        directory=child;
        copy(remaining,tail);
    }
}

} // namespace brook
