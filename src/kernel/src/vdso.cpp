#include "vdso.h"
#include "kvmclock.h"
#include "memory/address_space.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "panic.h"
#include "serial.h"
#include "string.h"
#include "pvclock_page.h"

namespace brook {
extern const uint8_t g_vdsoCodePage[4096];
static PhysicalAddress codePage, clockPage;
static bool privateClockPage = false;

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
        privateClockPage = true;
    }
    // Both initial references remain pinned for the kernel's lifetime. KVM
    // retains the clock reference even when no process maps the helper.
    SerialPuts("vDSO: clock helper ready\n");
}

bool VdsoPublishTscClock(uint64_t originTsc, uint64_t originNs, uint32_t mul, int8_t shift) {
    // Only the kernel-owned fallback page; a KVM-registered page is host-written.
    if (!clockPage || !privateClockPage || !mul) return false;
    auto* slots = reinterpret_cast<volatile BrookPvclockSlot*>(PhysToVirt(clockPage).raw());
    // Identical immutable records for every CPU (boot-verified synchronised
    // TSCs). Even version: never updated afterwards, so readers never retry.
    for (unsigned cpu = 0; cpu < BROOK_PVCLOCK_CPUS; ++cpu) {
        auto& c = slots[cpu].clock;
        c.tsc_timestamp = originTsc; c.system_time = originNs;
        c.tsc_to_system_mul = mul; c.tsc_shift = shift; c.flags = 1; // stable
        __atomic_store_n(&c.version, 2u, __ATOMIC_RELEASE);
    }
    auto* metadata = reinterpret_cast<volatile BrookPvclockMetadata*>(slots[0].reserved);
    metadata->stride = BROOK_PVCLOCK_STRIDE; metadata->cpus = BROOK_PVCLOCK_CPUS;
    metadata->features = BROOK_PVCLOCK_RDTSCP; metadata->version = BROOK_PVCLOCK_VERSION;
    // The helper validates magic last-to-first; publish it after everything.
    __atomic_store_n(&metadata->magic, uint32_t(BROOK_PVCLOCK_MAGIC), __ATOMIC_RELEASE);
    SerialPuts("vDSO: invariant-TSC clock records published\n");
    return true;
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
