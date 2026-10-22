#pragma once
#include <stdint.h>
#include "path_walk.h"

namespace brook {

struct RetainedNodeInfo { bool isDir=false, isSymlink=false; };

// Owns object and parent on success. The caller releases both, independently,
// even when they refer to the same object. Names are leaves, never authority
// for re-resolving the parent through a global pathname.
template<class Path> struct RetainedWalkResult {
    Path object{};
    Path parent{};
    char leaf[256]{};
    bool missing=false;
    bool trailing=false;
};

// Path{} is empty. Retain/Release operate on live references, including empty
// handles. Lookup(parent, oneName, out) returns a held child without following
// symlinks, entering a child mount only at that looked-up child (not at start).
// Parent(current, effectiveRoot, out) handles mount-root exits, including root
// clamping while crossing edges. Stat/Search/Readlink operate on held objects.
// Same compares attachment AND backend identity, not display names.
// The caller stabilizes topology and mutations through the eventual operation.
// No backend callback may recursively acquire that same namespace lock.
// root/start are borrowed live references. On error output is empty and every
// temporary reference is released; on success the result owns its references.
template<class Path, class Provider>
int WalkRetainedPath(const Path& root, const Path& start, const char* text,
                     RetainedWalkResult<Path>* output, Provider& fs,
                     PathWalkOptions options={})
{
    if(!output)return -22;
    *output={};
    if(!text || !text[0])return -2;
    auto length=[](const char* s) { uint32_t n=0;while(s[n])++n;return n; };
    auto copy=[](char* dst,const char* src) { while((*dst++=*src++)){} };
    if(length(text)>=512)return -36;
    struct Owned {
        Provider& fs;
        Path value{};
        ~Owned(){fs.Release(value);}
        void reset(const Path& p) { fs.Retain(p);fs.Release(value);value=p; }
        void take(Owned& other) { fs.Release(value);value=other.value;other.value={}; }
        Path detach() { Path p=value;value={};return p; }
    } current{fs};
    current.reset(text[0]=='/'?root:start);
    char remaining[512];copy(remaining,text);
    uint32_t links=0;
    for(;;) {
        const char* next=remaining;
        while(*next=='/')++next;
        if(!*next) {
            output->object=current.detach();
            output->trailing=remaining[0]=='/';
            return 0;
        }
        RetainedNodeInfo info{};
        if(int e=fs.Stat(current.value,info))return e;
        if(!info.isDir)return -20;
        if(int e=fs.Search(current.value))return e;
        char name[256];uint32_t count=0;
        while(*next && *next!='/') {
            if(count+1>=sizeof(name))return -36;
            name[count++]=*next++;
        }
        name[count]=0;
        char tail[512];copy(tail,next);
        const char* suffix=tail;while(*suffix=='/')++suffix;
        const bool final=!*suffix, trailing=final && tail[0];
        if(options.parentOnly && final) {
            output->parent=current.detach();
            copy(output->leaf,name);output->trailing=trailing;
            return 0;
        }
        const bool dot=name[0]=='.' && !name[1];
        const bool dotdot=name[0]=='.' && name[1]=='.' && !name[2];
        if(dot || dotdot) {
            if(dotdot && !fs.Same(current.value,root)) {
                Owned up{fs};
                if(int e=fs.Parent(current.value,root,up.value))return e;
                current.take(up);
            }
            copy(remaining,tail);continue;
        }
        Owned child{fs};
        const int error=fs.Lookup(current.value,name,child.value);
        if(error) {
            if(error!=-2 || !final || !options.allowMissingFinal)return error;
            if(trailing && options.missingTrailingError)return options.missingTrailingError;
            output->parent=current.detach();copy(output->leaf,name);
            output->missing=true;output->trailing=trailing;
            return 0;
        }
        RetainedNodeInfo childInfo{};
        if(int e=fs.Stat(child.value,childInfo))return e;
        if(childInfo.isSymlink && (!final || options.followFinal || trailing)) {
            if(++links>40)return -40;
            char target[512];
            const int bytes=fs.Readlink(child.value,target,sizeof(target));
            if(bytes<0)return bytes;
            if(!bytes)return -2;
            if(bytes>=static_cast<int>(sizeof(target)))return -36;
            for(int i=0;i<bytes;++i)if(!target[i])return -5;
            target[bytes]=0;
            if(uint32_t(bytes)+length(tail)>=sizeof(remaining))return -36;
            copy(remaining,target);copy(remaining+bytes,tail);
            if(target[0]=='/')current.reset(root);
            continue;
        }
        if(tail[0] && !childInfo.isDir)return -20;
        if(final) {
            output->object=child.detach();output->parent=current.detach();
            copy(output->leaf,name);output->trailing=trailing;return 0;
        }
        current.take(child);copy(remaining,tail);
    }
}

} // namespace brook
