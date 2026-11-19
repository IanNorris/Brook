#undef NDEBUG
#include <cassert>

#include "core_autodump_policy.h"

using namespace brook;

int main()
{
    for(int sig=1;sig<=64;++sig)
        assert(CoreAutoSignal(sig)==(sig==4 || sig==5 || sig==6 || sig==7 ||
                                     sig==8 || sig==11 || sig==31));
    const CoreResourceLimits defaults{};
    assert(CoreAutoBudget(defaults)==CoreAutoMaxBytes);
    assert(CoreAutoBudget({4096,8192})==4096);
    assert(CoreAutoEligible(CoreAutoMode::Uid1000,false,1000,1000,1000,11,defaults));
    assert(!CoreAutoEligible(CoreAutoMode::Uid1000,false,1000,1000,1000,11,{0,0}));
    assert(!CoreAutoEligible(CoreAutoMode::Uid1000,false,1000,0,1000,11,defaults));
    assert(!CoreAutoEligible(CoreAutoMode::Uid1000,false,0,0,0,11,defaults));
    assert(CoreAutoEligible(CoreAutoMode::AllUsers,false,0,0,0,11,defaults));
    assert(!CoreAutoEligible(CoreAutoMode::AllUsers,true,0,0,0,11,defaults));
    assert(!CoreAutoEligible(CoreAutoMode::AllUsers,false,1000,1000,1000,9,defaults));
    assert(CoreAutoEligible(CoreAutoMode::Uid1000,false,1000,1000,1000,0,defaults));
    assert(!CoreAutoEligible(CoreAutoMode::Off,false,1000,1000,1000,0,defaults));

    assert(!CoreAutoExpired(700,20699));
    assert(CoreAutoExpired(700,20700));
    assert(CoreAutoExpired(~uint64_t{0}-19999,0));

    CoreAutoRate rate;
    assert(rate.Admit(10,0));
    assert(!rate.Admit(10,59999));
    assert(rate.Admit(11,59999));
    assert(rate.Admit(10,60000));
    CoreAutoRate full;
    for(uint64_t i=1;i<=64;++i)assert(full.Admit(i,100));
    assert(!full.Admit(65,200));
    assert(full.Admit(65,60100));

    CoreAutoRetention space;
    bool oldest=false;
    assert(space.add(CoreAutoMaxBytes,4,oldest) && oldest);
    assert(space.add(CoreAutoMaxBytes,5,oldest) && !oldest);
    assert(space.add(CoreAutoMaxBytes,2,oldest) && oldest && space.oldestTime==2);
    assert(!space.needsRotation(CoreAutoMaxBytes));
    assert(space.add(CoreAutoMaxBytes,3,oldest) && !oldest && space.oldestTime==2);
    assert(space.needsRotation(1));
    CoreAutoRetention count;
    for(uint32_t i=0;i<4;++i)assert(count.add(1,10-i,oldest) && oldest);
    assert(!count.needsRotation(1));
    assert(count.add(1,20,oldest) && !oldest && count.needsRotation(1));
    assert(!count.add(~uint64_t{0},20,oldest));
}
