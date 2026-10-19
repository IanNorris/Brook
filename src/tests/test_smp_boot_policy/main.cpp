#include "../../kernel/src/smp_boot_policy.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"SMP_BOOT_POLICY line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
int main() {
    using namespace brook;
    uint8_t out[8]{};
    const uint8_t ids[]={21,9,44,9,21,63};
    CHECK(SmpOrderApicIds(ids,6,44,out,8)==4);
    CHECK(out[0]==44 && out[1]==21 && out[2]==9 && out[3]==63);
    CHECK(SmpOrderApicIds(ids,6,7,out,8)==5 && out[0]==7);
    CHECK(SmpOrderApicIds(nullptr,0,7,out,8)==1 && out[0]==7);
    CHECK(!SmpOrderApicIds(ids,6,44,out,3));
    CHECK(!SmpOrderApicIds(ids,6,44,out,0));
    CHECK(!SmpOrderApicIds(nullptr,1,44,out,8));
    // A late previous AP cannot admit the next slot. Timeout permanently
    // closes admission: even its eventual matching acknowledgement is stale.
    for (uint32_t failed=1;failed<8;++failed) {
        SmpBootAdmission admission;
        for (uint32_t i=1;i<failed;++i) {
            CHECK(admission.CanStart(i));
            CHECK(!admission.Accept(i,i));
            CHECK(admission.Accept(i,i+1));
        }
        CHECK(admission.Count()==failed && admission.CanStart(failed));
        CHECK(!admission.Accept(failed,failed));
        CHECK(!admission.CanStart(failed+1));
        admission.Stop();
        CHECK(!admission.CanStart(failed));
        CHECK(!admission.Accept(failed,failed+1));
        CHECK(!admission.CanStart(failed+1) && admission.Count()==failed);
    }
    SmpBootAdmission all;
    for (uint32_t i=1;i<8;++i) CHECK(all.Accept(i,i+1));
    CHECK(all.Count()==8);
    std::puts("SMP_BOOT_POLICY_PASS BSP ordering, sparse APIC IDs, duplicates, bounded topology, wrong/late acknowledgements and stopped suffix");
}
