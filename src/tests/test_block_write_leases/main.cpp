#include <cassert>
#include <cstdint>
#include "block_write_leases.h"
using namespace brook;
int main(){
    BlockWriteLeases leases;
    const BlockWriteLease plan[]={{1,0,0,100},{1,2,200,100},{1,0,900,100}};
    assert(!leases.Install(plan,3,false) && leases.Count()==0);
    assert(leases.Install(plan,3,true) && leases.Count()==3);
    assert(leases.References(1) && leases.References(2));
    assert(!leases.References(0) && !leases.References(4));
    assert(leases.AllowOrdinary(1,100,100));
    assert(!leases.AllowOrdinary(1,99,2));
    assert(!leases.AllowOrdinary(1,299,2));
    assert(!leases.AllowOrdinary(1,900,1));
    assert(leases.AllowOrdinary(4,200,100));
    assert(!leases.AllowOrdinary(1,UINT64_MAX,2));
    assert(leases.AllowOwner(1,2,200,100));
    assert(leases.AllowOwner(1,2,250,50));
    assert(!leases.AllowOwner(1,2,199,2));
    assert(!leases.AllowOwner(1,2,299,2));
    assert(!leases.AllowOwner(1,3,200,1));
    assert(!leases.AllowOwner(4,2,200,1));
    assert(!leases.AllowOwner(1,0,0,1));
    assert(!leases.AllowOwner(1,2,UINT64_MAX,2));
    const BlockWriteLease bad[]={{1,3,300,10},{1,4,95,10}};
    assert(!leases.Install(bad,2,true) && leases.Count()==3);
    assert(leases.AllowOrdinary(1,300,10)); // no partial publication
    const BlockWriteLease overflow[]={{1,3,UINT64_MAX,2}};
    assert(!leases.Install(overflow,1,true));
    const BlockWriteLease duplicate[]={{1,3,400,10},{1,4,409,10}};
    assert(!leases.Install(duplicate,2,true) && leases.Count()==3);
    const BlockWriteLease adjacent[]={{1,3,300,10},{1,4,310,10}};
    assert(leases.Install(adjacent,2,true) && leases.Count()==5);
    assert(!leases.AllowOwner(1,3,309,2));
    return 0;
}
