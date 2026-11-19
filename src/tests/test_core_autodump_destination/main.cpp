#include <cassert>
#include <map>
#include <string>
#include "../../kernel/src/core_autodump.cpp"

namespace {
struct Entry {
    std::string path;
    uint16_t mode;
    uint32_t uid, gid;
};
struct Tree {
    std::map<std::string,Entry> entries;
    Entry& add(const std::string& path,uint16_t mode,uint32_t uid=0,uint32_t gid=0) {
        return entries.try_emplace(path,Entry{path,mode,uid,gid}).first->second;
    }
};
Tree initial, target;
Tree* currentNamespace=&target;
unsigned roots;
constexpr uint16_t Dir=0040000, Reg=0100000;
Entry& EntryAt(const brook::VfsPath& path) { return *reinterpret_cast<Entry*>(path.object); }
const std::string Child(const brook::VfsPath& start,const char* name) {
    if(*name=='/')return name;
    const auto& parent=EntryAt(start).path;
    return parent=="/"?parent+name:parent+"/"+name;
}
brook::VfsPath Reference(Tree& tree,Entry& entry) {return {&tree,reinterpret_cast<brook::VfsObject*>(&entry)};}
}

namespace brook {
int VfsPathRoot(VfsPath* out) {
    assert(currentNamespace==&target);
    ++roots;
    *out=Reference(initial,initial.entries.at("/"));
    return 0;
}
void VfsPathRelease(VfsPath) {}
void VfsPathRetain(const VfsPath&) {}
int VfsPathResolve(const VfsPath& root,const VfsPath& start,const char* name,VfsPath* out,bool follow,bool) {
    assert(root.attachment==&initial && start.attachment==&initial && !follow);
    const auto it=initial.entries.find(Child(start,name));
    if(it==initial.entries.end())return -2;
    *out=Reference(initial,it->second);
    return 0;
}
int VfsMkdirAt(const VfsPath& root,const VfsPath& start,const char* name,uint16_t mode,uint16_t mask) {
    assert(root.attachment==&initial && start.attachment==&initial && !mask);
    const auto path=Child(start,name);
    if(initial.entries.count(path))return -17;
    initial.add(path,Dir|mode);
    return 0;
}
int VfsPathStat(const VfsPath& path,VnodeStat* st) {
    const auto& entry=EntryAt(path);
    *st={};
    st->mode=entry.mode;st->uid=entry.uid;st->gid=entry.gid;
    st->isDir=(entry.mode&0170000)==Dir;st->nlink=1;
    return 0;
}
int VfsStatAt(const VfsPath& root,const VfsPath& start,const char* name,VnodeStat* st,bool follow) {
    VfsPath path;
    if(int error=VfsPathResolve(root,start,name,&path,follow,false))return error;
    return VfsPathStat(path,st);
}
Vnode* VfsPathOpen(const VfsPath& path,int,int*) {return reinterpret_cast<Vnode*>(&EntryAt(path));}
int VfsChown(Vnode* node,uint32_t uid,uint32_t gid) {
    auto& entry=*reinterpret_cast<Entry*>(node);
    entry.uid=uid;entry.gid=gid;
    return 0;
}
void VfsClose(Vnode*) {}
int VfsReaddir(Vnode*,DirEntry*,uint32_t*) {return 0;}
int VfsRemoveAt(const VfsPath& root,const VfsPath& start,const char* name,bool) {
    assert(root.attachment==&initial && start.attachment==&initial);
    return initial.entries.erase(Child(start,name))?0:-2;
}
int VfsRenameNoReplaceAt(const VfsPath& root,const VfsPath& source,const char* oldName,const VfsPath& dest,const char* name) {
    assert(root.attachment==&initial && source.attachment==&initial && dest.attachment==&initial);
    const auto to=Child(dest,name);
    if(initial.entries.count(to))return -17;
    auto old=initial.entries.extract(Child(source,oldName));
    assert(!old.empty());
    auto& entry=initial.add(to,old.mapped().mode,old.mapped().uid,old.mapped().gid);
    assert(entry.uid==EntryAt(dest).uid);
    return 0;
}
uint64_t ApicTickCount(){return 101;}
uint64_t RtcNow(){return 200;}
void AddressSpaceUnref(AddressSpaceState*) {}
int CoreDumpWriteSnapshot(CoreDumpSnapshot&,const char* path,bool full,const CoreDumpDestination& dest) {
    assert(!full && dest.root->attachment==&initial && dest.directory->attachment==&initial);
    assert(EntryAt(*dest.directory).uid==0 && EntryAt(*dest.directory).mode==(Dir|0700));
    assert(dest.setOwner && dest.uid==1000 && dest.gid==1234);
    assert(dest.reserve(dest.context,4096)==0);
    initial.add(Child(*dest.directory,path),Reg|0600,dest.uid,dest.gid);
    return 0;
}
}

int main() {
    using namespace brook;
    for(const char* dir:{"/","/data","/data/home","/data/home/.local","/data/home/.local/share","/data/home/.local/share/brook"})
        initial.add(dir,Dir|0755);
    constexpr char cores[]="/data/home/.local/share/brook/cores";
    const std::string pending=std::string(cores)+"/.brook-private";
    target.add("/",Dir|0755);
    target.add("/data/home",Dir|0755);
    initial.add(cores,Dir|0700); // previous boot's root-only destination
    const auto targetBefore=target.entries.size();
    g_job.snapshot.info.uid=1000;g_job.snapshot.info.gid=1234;g_job.snapshot.info.pid=41;
    g_job.started=100;g_job.byteLimit=CoreAutoMaxBytes;
    char comm[16]="sandbox",name[80]{},display[80]{};
    assert(WriteJob(comm,name,display)==0);
    assert(roots==1 && target.entries.size()==targetBefore);
    assert(initial.entries.at(cores).uid==1000 && initial.entries.at(cores).mode==(Dir|0700));
    assert(initial.entries.at(pending).uid==0 && initial.entries.at(pending).mode==(Dir|0700));
    const auto& result=initial.entries.at(std::string(cores)+"/sandbox.41.200.core");
    assert(result.uid==1000 && result.gid==1234 && result.mode==(Reg|0600));
    assert(!initial.entries.count(pending+"/pending"));

    initial.entries.at(pending).uid=1000;
    assert(WriteJob(comm,name,display)==-13);
    assert(std::string(g_job.snapshot.result.stage)=="staging-directory");
}
