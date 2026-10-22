#include "retained_path_walk.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>

// The graph is keyed by attachment/inode identity; there is deliberately no
// global pathname resolver that could make these tests pass by reconstruction.
struct Path { int mount=0,node=0; };
static bool operator==(Path a,Path b){return a.mount==b.mount && a.node==b.node;}
struct Node { bool dir=true,link=false,search=true;int refs=0,error=0;std::string target; };
struct Model {
    std::map<std::pair<int,int>,Node> nodes;
    std::map<std::pair<std::pair<int,int>,std::string>,Path> children;
    std::map<std::pair<int,int>,Path> parents, mounts;
    std::map<int,Path> edges;
    int failLookup=0;
    static std::pair<int,int> key(Path p){return {p.mount,p.node};}
    Node& at(Path p){return nodes.at(key(p));}
    void Retain(Path p){if(p.node)++at(p).refs;}
    void Release(Path p){if(p.node){assert(at(p).refs>0);--at(p).refs;}}
    bool Same(Path a,Path b){return a==b;}
    int Stat(Path p,brook::RetainedNodeInfo& info){auto& n=at(p);if(n.error)return n.error;info={n.dir,n.link};return 0;}
    int Search(Path p){return at(p).search?0:-13;}
    int Lookup(Path p,const char* name,Path& out){
        if(failLookup)return failLookup;
        auto i=children.find({key(p),name});if(i==children.end())return -2;
        out=i->second;auto mount=mounts.find(key(out));if(mount!=mounts.end())out=mount->second;
        Retain(out);return 0;
    }
    int Parent(Path p,Path root,Path& out){
        while(p.node==1 && edges.count(p.mount)) {
            p=edges.at(p.mount);
            if(Same(p,root)){out=p;Retain(out);return 0;}
        }
        auto i=parents.find(key(p));out=i==parents.end()?p:i->second;Retain(out);return 0;
    }
    int Readlink(Path p,char* out,uint32_t size){auto& s=at(p).target;auto n=s.size()<size?s.size():size;std::memcpy(out,s.data(),n);return n;}
    void add(Path parent,const char* name,Path child,bool dir=true){nodes[key(child)].dir=dir;children[{key(parent),name}]=child;parents[key(child)]=parent;}
    void link(Path parent,const char* name,Path child,const char* target){add(parent,name,child,false);at(child).link=true;at(child).target=target;}
    int refs(){int n=0;for(auto& p:nodes)n+=p.second.refs;return n;}
};
int main(){
    Model fs;Path root{1,1},held{1,2},file{1,3},replacement{1,4},poison{1,5};fs.nodes[Model::key(root)]={};
    fs.add(root,"dir",held);fs.add(held,"marker",file,false);
    fs.Retain(root);fs.Retain(held);
    using Result=brook::RetainedWalkResult<Path>;
    auto run=[&](Path effectiveRoot,Path start,const std::string& text,int error,Path expected=Path{},brook::PathWalkOptions options=brook::PathWalkOptions{}){
        const int before=fs.refs();Result result{};
        assert(brook::WalkRetainedPath(effectiveRoot,start,text.c_str(),&result,fs,options)==error);
        if(!error)assert(result.object==expected);
        else {assert(result.object==Path{});assert(result.parent==Path{});assert(!result.leaf[0]);}
        fs.Release(result.object);fs.Release(result.parent);assert(fs.refs()==before);
    };
    run(root,held,"marker",0,file);run(root,held,".",0,held);run(root,held,"..",0,root);
    // Rename and place a hostile replacement at the old path.
    fs.children.erase({Model::key(root),"dir"});fs.children[{Model::key(root),"moved"}]=held;
    fs.add(root,"dir",replacement);fs.add(replacement,"marker",poison,false);
    run(root,held,"marker",0,file);run(root,root,"/dir/marker",0,poison);
    // Overmount affects newly looked-up children, never the retained start.
    Path mounted{2,1},newFile{2,3};fs.nodes[Model::key(mounted)]={};fs.add(mounted,"marker",newFile,false);
    fs.edges[2]=held;fs.mounts[Model::key(held)]=mounted;
    run(root,held,"marker",0,file);run(root,root,"/moved/marker",0,newFile);
    run(root,mounted,"..",0,root);run(root,mounted,"../dir/marker",0,poison);
    // A nested attachment at an attachment root still crosses both edges.
    Path upper{3,1};fs.nodes[Model::key(upper)]={};fs.edges[3]=mounted;
    run(root,upper,"..",0,root);
    // Effective root clamping includes intermediate edges; an outside dirfd
    // remains usable, matching chroot's non-revocation semantics.
    run(mounted,mounted,"../../",0,mounted);run(held,mounted,"..",0,held);
    run(held,root,"dir/marker",0,poison);run(held,root,"/marker",0,file);
    fs.link(held,"relative",{1,6},"marker");fs.link(held,"absolute",{1,7},"/dir/marker");
    fs.link(held,"up",{1,8},"../dir/marker");fs.link(held,"loop",{1,9},"loop");
    run(root,held,"relative",0,file);run(root,held,"absolute",0,poison);run(root,held,"up",0,poison);
    run(held,held,"/relative",0,file);run(root,held,"relative",0,{1,6},{false});
    run(root,held,"relative/",-20,{},{false});run(root,held,"loop",-40);
    run(root,held,"marker/..",-20);run(root,held,"marker/",-20);
    fs.at(held).search=false;run(root,held,".",-13);run(root,held,"..",-13);run(root,root,"/moved/",0,mounted);fs.at(held).search=true;
    // A detached directory does not need a name to resolve its held objects.
    fs.children.erase({Model::key(root),"moved"});run(root,held,"marker",0,file);
    run(root,held,"absent",-2);run(root,held,"absent/",-21,{},{true,true,false,-21});
    int before=fs.refs();Result result{};
    assert(brook::WalkRetainedPath(root,held,"absent",&result,fs,{true,true})==0);
    assert(result.missing && result.parent==held && result.object==Path{} && !std::strcmp(result.leaf,"absent"));
    fs.Release(result.parent);assert(fs.refs()==before);
    assert(brook::WalkRetainedPath(root,held,"relative/",&result,fs,{false,false,true})==0);
    assert(result.parent==held && result.trailing && !std::strcmp(result.leaf,"relative"));fs.Release(result.parent);assert(fs.refs()==before);
    // Missing parent, unsupported backend, permission/stat errors and capacity.
    run(root,held,"absent/child",-2);fs.failLookup=-95;run(root,held,"marker",-95);fs.failLookup=0;
    fs.at(file).error=-5;run(root,held,"marker",-5);fs.at(file).error=0;
    run(root,held,std::string(256,'x'),-36);run(root,held,std::string(512,'x'),-36);run(root,held,"",-2);
    fs.Release(held);fs.Release(root);assert(!fs.refs());
    std::puts("RETAINED_PATH_WALK_PASS identity/rename/overmount/mount-parent/root/symlinks/lifetimes/errors");
}
