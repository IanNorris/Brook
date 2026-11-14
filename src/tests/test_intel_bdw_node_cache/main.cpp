#include "../../kernel/src/intel_bdw_node_cache.h"
#include <cstdio>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
namespace brook { BdwNodeCache& BdwIntelNodeCache(){static BdwNodeCache c{};return c;} }
using namespace brook;
int main(){
    BdwNodeCache c{};unsigned reads=0;bool device=true;
    auto check=[&]{++reads;return device;};
    CHECK(BdwNodeCachedCheck(c,1000,check) && reads==1);
    CHECK(BdwNodeCachedCheck(c,1050,check) && reads==1 && c.hits==1);     // reused within the window
    CHECK(BdwNodeCachedCheck(c,1099,check) && reads==1);
    CHECK(BdwNodeCachedCheck(c,1100,check) && reads==2);                   // expired: live check again
    device=false;
    CHECK(BdwNodeCachedCheck(c,1150,check) && reads==2);                   // still cached (worker checks live)
    CHECK(!BdwNodeCachedCheck(c,1300,check) && reads==3);                  // expired, failing check
    CHECK(!BdwNodeCachedCheck(c,1301,check) && reads==4);                  // failures are never cached
    device=true;CHECK(BdwNodeCachedCheck(c,1302,check) && reads==5);
    CHECK(BdwNodeCachedCheck(c,900,check) && reads==6);                    // clock went backwards: recheck
    BdwNodeCacheSetEnabled(c,false);
    CHECK(BdwNodeCachedCheck(c,905,check) && reads==7);
    CHECK(BdwNodeCachedCheck(c,906,check) && reads==8);                    // disabled: always live
    BdwNodeCacheSetEnabled(c,true);
    CHECK(BdwNodeCachedCheck(c,907,check) && reads==9 && BdwNodeCachedCheck(c,908,check) && reads==9);
    std::puts("test_intel_bdw_node_cache ok");return 0;
}
