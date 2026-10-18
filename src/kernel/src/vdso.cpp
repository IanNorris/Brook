#include "vdso.h"
#include "kvmclock.h"
#include "memory/address_space.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "panic.h"
#include "serial.h"
#include "string.h"

namespace brook {
extern const uint8_t g_vdsoCodePage[4096];
static PhysicalAddress codePage, clockPage;

void VdsoInit() {
    codePage = PmmAllocPage(MemTag::KernelData);
    if (!codePage) return;
    memcpy(reinterpret_cast<void*>(PhysToVirt(codePage).raw()), g_vdsoCodePage, 4096);
    clockPage = KvmClockPublicPage();
    if (!clockPage) {
        clockPage = PmmAllocPage(MemTag::KernelData);
        if (!clockPage) { PmmUnrefPage(codePage); codePage = {}; return; }
        // Unsupported hosts still get the same ELF ABI, using syscall fallback.
        memset(reinterpret_cast<void*>(PhysToVirt(clockPage).raw()), 0, 4096);
    }
    // Both initial references remain pinned for the kernel's lifetime. KVM
    // retains the clock reference even when no process maps the helper.
    SerialPuts("vDSO: clock helper ready\n");
}

uint64_t VdsoInstall(AddressSpaceState* space, uint64_t random) {
    if (!codePage || !clockPage || !space || space->kernel || space->published) return 0;
    constexpr uint64_t floor = 0x700000000000ULL, ceiling = 0x710000000000ULL;
    const uint64_t hint = floor + ((random & 0x7ffffffULL) << 13);
    const uint64_t base = MappingRangeFindGap(space->ranges, hint, 8192, floor, ceiling);
    if (!base || VmmReadLeaf(space->pageTable, VirtualAddress(base)) ||
        VmmReadLeaf(space->pageTable, VirtualAddress(base + 4096))) return 0;
    MappingRangePlan plan;
    MappingRange code{base,base+4096,0,0,nullptr,5,5,MappingKind::Image,
                       MappingSharing::Shared,MappingLeafOwnership::Managed};
    MappingRange clock{base+4096,base+8192,0,0,nullptr,1,1,MappingKind::Image,
                        MappingSharing::Shared,MappingLeafOwnership::Managed};
    // Allocation failures occur before any metadata/leaf publication. Empty
    // intermediate tables left by preparation remain owned by AS teardown.
    if (space->ranges.nextIdentity > ~uint64_t{0} - 2 || !plan.Prepare(2) ||
        !VmmPrepareRange(space->pageTable, base, base+8192)) return 0;
    if (!plan.Insert(space->ranges, code) || !plan.Insert(space->ranges, clock))
        KernelPanic("vDSO: preallocated metadata insertion failed");
    const PhysicalAddress pages[] = {codePage,clockPage};
    for (unsigned i = 0; i < 2; ++i) {
        PmmRefPage(pages[i]); // The new managed user PTE owns its own reference.
        const auto result = VmmTryMapPage(space->pageTable, VirtualAddress(base+i*4096),
            pages[i], VMM_USER | (i ? VMM_NO_EXEC : 0), MemTag::User, space->allocationPid);
        if (result != VmmMapResult::Mapped)
            KernelPanic("vDSO: prepared exclusive page publication failed");
    }
    return base;
}
}
