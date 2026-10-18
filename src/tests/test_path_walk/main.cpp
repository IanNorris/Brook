#include "path_walk.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdio>

struct Node { bool isDir,isSymlink,searchable; int error; std::string target;
    Node(bool d=false,bool l=false,bool s=true,int e=0,std::string t={}) : isDir(d),isSymlink(l),searchable(s),error(e),target(t) {}
};
struct Model {
    std::map<std::string,Node> nodes;
    std::vector<std::string> searches;
    int Stat(const char* p,Node& n) { auto i=nodes.find(p);if(i==nodes.end())return -2;n=i->second;return n.error; }
    int Search(const char* p,const Node& n) { searches.emplace_back(p);return n.searchable?0:-13; }
    int Readlink(const char* p,char* b,uint32_t size) { auto& t=nodes.at(p).target;auto n=t.size()<size?t.size():size;std::memcpy(b,t.data(),n);return n; }
};
int main() {
    Model fs;
    fs.nodes["/"]={true};fs.nodes["/public"]={true};fs.nodes["/public/file"]={false};
    fs.nodes["/locked"]={true,false,false};fs.nodes["/locked/file"]={false};
    fs.nodes["/private"]={true,false,false};fs.nodes["/private/deep"]={true};
    fs.nodes["/public/jump"]={false,true,true,0,"../private/deep"};
    fs.nodes["/public/absolute"]={false,true,true,0,"/private/deep"};
    fs.nodes["/public/good"]={false,true,true,0,"/public"};
    fs.nodes["/loop"]={false,true,true,0,"/loop"};
    fs.nodes["/io"]={false,false,true,-5};
    auto check=[&](const std::string& input,int expected,const char* result="",brook::PathWalkOptions opts={}) {
        char output[512]="sentinel";fs.searches.clear();
        assert(brook::WalkPath<Node>(input.c_str(),output,sizeof(output),fs,opts)==expected);
        assert(!std::strcmp(output,result));
    };
    check("/public/file",0,"/public/file");
    check("/locked",0,"/locked");check("/locked/",0,"/locked");
    check("/locked/.",-13);check("/locked/../public/file",-13);
    check("/public/file/../file",-20);check("/public/file/",-20);
    check("/public/jump/../file",-13);check("/public/absolute/file",-13);
    check("/public/good/../public/file",0,"/public/file");
    check("/public/good",0,"/public/good",{false});
    check("/public/good/",0,"/public",{false});
    check("/loop",-40);check("/io",-5);
    check("/absent/file",-2);check("/public/new",0,"/public/new",{true,true});
    check("/public/new/",-21,"",{true,true,false,-21});
    check("/locked/new",-13,"",{true,true});
    check("/public/new/",0,"/public/new/",{false,true,true});
    check("/public/.",0,"/public/.",{false,false,true});
    check("/public/..",0,"/public/..",{false,false,true});
    check("/public/jump/new",-13,"",{false,true,true});
    check("/public/"+std::string(256,'x'),-36);
    check("/"+std::string(511,'x'),-36);
    char tiny[3];assert(brook::WalkPath<Node>("/public",tiny,sizeof(tiny),fs)==-36);
    std::puts("PATH_WALK_PASS");
}
