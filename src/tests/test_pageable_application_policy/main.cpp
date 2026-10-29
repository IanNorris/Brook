#include <assert.h>
#include <initializer_list>
#include "../../kernel/src/memory/pageable_application_policy.h"
int main(){
    brook::PageableApplicationPolicy policy;
    bool active=false;
    assert(!policy.ReclaimAllowed(active,8193) && !active);
    assert(policy.ReclaimAllowed(active,8192) && active);
    assert(policy.ReclaimAllowed(active,16383) && active);
    assert(!policy.ReclaimAllowed(active,16384) && !active);
    assert(policy.ReclaimAllowed(active,0) && active);
    policy.headroomPages=0;
    assert(policy.ReclaimAllowed(active,100000) && !active);
    assert(!policy.Configure("/test/compiler",14,4096,65537));
    assert(!policy.Matches("/test/compiler",65536,3,0x22));
    assert(policy.Configure("/test/compiler",14,4096));
    assert(policy.Matches("/test/compiler",65536,3,0x22));
    assert(policy.Matches("/test/compiler",16*1024*1024,3,0x22));
    assert(!policy.Matches("/test/compiler-alias",65536,3,0x22));
    assert(!policy.Matches("/test/compile",65536,3,0x22));
    assert(!policy.Matches("/test/compiler",4096,3,0x22));
    assert(!policy.Matches("/test/compiler",65537,3,0x22));
    assert(!policy.Matches("/test/compiler",32*1024*1024,3,0x22));
    for(auto flag:{0x21u,0x32u,0x100022u,0x8022u,0x62u})
        assert(!policy.Matches("/test/compiler",65536,3,flag));
    for(unsigned prot=0;prot<8;++prot)
        assert(policy.Matches("/test/compiler",65536,prot,0x22)==(prot==3));
    assert(!policy.Configure("relative",8,4096));
    assert(!policy.Matches("/test/compiler",65536,3,0x22));
    assert(!policy.Configure("/x\0y",4,4096));
    assert(!policy.Configure("/test/compiler",14,0));
    assert(!policy.Configure("/test/compiler",14,65537));
    char longPath[256];for(auto& c:longPath)c='x';longPath[0]='/';
    assert(!policy.Configure(longPath,256,4096));
    assert(policy.Configure(longPath,255,4096));
    longPath[255]=0;assert(policy.Matches(longPath,65536,3,0x22));
}
