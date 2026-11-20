#include "process_resident.h"

#include "memory/user_mapping.h"
#include "scheduler.h"

namespace brook {

int ProcessMeasureResident(uint16_t pid, uint64_t incarnation, ResidentPages& resident)
{
    resident = {};
    bool exists = false;
    auto* space = SchedulerReferenceProcessAddressSpace(pid, incarnation, &exists);
    if (!exists) return -3;
    if (!space) return 0;

    int result = 0;
    KMutexLock(&space->mutation);
    for (auto* node = space->ranges.head; node; node = node->next) {
        const auto& range = node->range;
        VmmRangeStats pages{};
        if (ResidentPages::ChargesRss(range) &&
            !VmmMeasureRange(space->pageTable, range.begin, range.end, false, &pages)) {
            result = -5;
            break;
        }
        resident.Add(range, pages.backedPages);
    }
    KMutexUnlock(&space->mutation);
    AddressSpaceUnref(space);
    return result;
}

}
