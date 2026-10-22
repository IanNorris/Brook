#include "vfs_object.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
static std::atomic<unsigned> destroyed{0};
static void destroy(brook::VfsObject*){++destroyed;}
static const brook::VfsObjectOps ops={destroy,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
static void expectTrap(unsigned mode){
    pid_t child=fork();assert(child>=0);
    if(!child){
        rlimit limit{0,0};setrlimit(RLIMIT_CORE,&limit);
        brook::VfsObject object{&ops,nullptr,1,mode>=2?0u:~uint32_t{0}};
        if(mode==0 || mode==2)brook::VfsObjectRetain(&object);
        else if(mode==1)brook::VfsObjectTryRetain(&object);
        else brook::VfsObjectRelease(&object);
        _exit(20);
    }
    int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGILL);
}
int main(){
    brook::VfsObject object{&ops,nullptr,1,1};
    std::vector<std::thread> workers;
    for(unsigned i=0;i<8;++i)workers.emplace_back([&]{for(unsigned n=0;n<20000;++n){brook::VfsObjectRetain(&object);brook::VfsObjectRelease(&object);}});
    for(auto& worker:workers)worker.join();
    assert(object.references==1 && !destroyed);
    assert(brook::VfsObjectTryRetain(&object));brook::VfsObjectRelease(&object);brook::VfsObjectRelease(&object);
    assert(object.references==0 && destroyed==1 && !brook::VfsObjectTryRetain(&object));
    assert(!brook::VfsObjectTryRetain(nullptr));
    for(unsigned i=0;i<4;++i)expectTrap(i);
    std::puts("VFS_OBJECT_REFS_PASS concurrent-retain/final-release/no-resurrection/overflow/underflow");
}
