#include <atomic>
#include <cassert>
#include <thread>
#include "../../kernel/src/memory/paging_admission.h"
#include "../../kernel/src/memory/mapping_ranges.h"
int main() {
    brook::PagingAdmission shared;
    assert(shared.Share());assert(shared.Share());assert(!shared.ClaimExclusive());
    brook::PagingAdmission exclusive;
    assert(exclusive.ClaimExclusive());assert(exclusive.ClaimExclusive());assert(!exclusive.Share());
    assert(exclusive.Exclusive());
    brook::MappingRangeNode node{};brook::MappingRangeTable table{};table.head=&node;
    node.range.kind=brook::MappingKind::Pageable;node.range.begin=0x10000;node.range.end=0x20000;
    assert(!brook::MappingRangeRemovalAllowed(table,0x10000,0x20000));
    assert(!brook::MappingRangeRemovalAllowed(table,0,0x30000));
    assert(brook::MappingRangeRemovalAllowed(table,0x20000,0x30000));
    assert(!brook::MappingRangeRemovalAllowed(table,0x10000,0x11000));
    assert(!brook::MappingRangeRemovalAllowed(table,0x11000,0x20000));
    for(unsigned iteration=0;iteration<200;++iteration) {
        brook::PagingAdmission admission;std::atomic<bool> ready{false};bool share=false,claim=false;
        std::thread a([&]{while(!ready.load()){}share=admission.Share();});
        std::thread b([&]{while(!ready.load()){}claim=admission.ClaimExclusive();});
        ready=true;a.join();b.join();
        assert(share!=claim);assert(admission.Exclusive()==claim);
        if(claim)assert(!admission.Share());else assert(!admission.ClaimExclusive());
    }
}
