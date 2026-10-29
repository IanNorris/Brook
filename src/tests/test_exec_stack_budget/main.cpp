#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "../../kernel/src/exec_limits.h"
using namespace brook;
// Independently simulate downward pushes, including exact alignment, instead
// of copying the conservative summed bound used for physical preallocation.
static uint64_t consumed(unsigned argc,unsigned envc,uint64_t args,uint64_t env,uint64_t execfn,bool vdso){
    uint64_t top=0x1000000,sp=top-8;
    sp-=16;sp-=7;sp-=execfn;sp-=env;sp-=args;sp&=~uint64_t{15};
    unsigned words=2*(19+(vdso?1:0))+(envc+1)+(argc+1)+1;
    if(words&1)sp-=8;
    sp-=2*(19+(vdso?1:0))*8;sp-=(envc+1)*8;sp-=(argc+1)*8;sp-=8;
    assert(!(sp&15));return top-sp;
}
int main(){
    static_assert(EXEC_INITIAL_STACK_PAGES==42);
    for(unsigned argc:{0u,1u,unsigned(EXEC_MAX_ARGS)})for(unsigned envc:{0u,1u,unsigned(EXEC_MAX_ENV)})for(bool vdso:{false,true}){
        uint64_t args=argc?EXEC_ARG_BYTES:0,env=envc?EXEC_ENV_BYTES:0;
        uint64_t execfn=argc?args-(argc-1):0;
        assert(consumed(argc,envc,args,env,execfn,vdso)<=EXEC_INITIAL_STACK_BYTES);
        assert(consumed(argc,envc,args,env,execfn,vdso)<=EXEC_INITIAL_STACK_PAGES*4096);
    }
    std::string largeArg(EXEC_ARG_BYTES-EXEC_MAX_ARGS,'a');
    std::string largeEnv(EXEC_ENV_BYTES-EXEC_MAX_ENV,'e');
    std::vector<const char*> argv(EXEC_MAX_ARGS,""),envp(EXEC_MAX_ENV,"");argv[0]=largeArg.c_str();envp[0]=largeEnv.c_str();
    assert(ExecArgumentsFit(argv.size(),argv.data(),envp.size(),envp.data()));
    largeArg+='a';argv[0]=largeArg.c_str();assert(!ExecArgumentsFit(argv.size(),argv.data(),envp.size(),envp.data()));
    assert(!ExecArgumentsFit(EXEC_MAX_ARGS+1,argv.data(),0,nullptr));
    assert(!ExecArgumentsFit(0,nullptr,EXEC_MAX_ENV+1,envp.data()));
}
