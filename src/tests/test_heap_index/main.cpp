#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include "memory/heap.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"

// Run the production allocator with its real tags, bins, poison and IRQ-lock
// implementation. Only the virtual region, page provider and serial are fake.
static constexpr uint64_t Region = BROOK_HEAP_HOST_REGION;
static constexpr uint64_t RegionBytes = 64 * 1024 * 1024;
static uint64_t mappings[RegionBytes / 4096]{};
static bool frames[RegionBytes / 4096]{};
static unsigned nextFrame = 1, liveFrames = 0;
static int failMapAfter = -1;
namespace brook {
PhysicalAddress PmmAllocPage(MemTag, uint16_t) {
    assert(nextFrame < RegionBytes / 4096);
    frames[nextFrame] = true;
    ++liveFrames;
    return PhysicalAddress(uint64_t(nextFrame++) * 4096);
}
void PmmFreePage(PhysicalAddress address) {
    const unsigned index = address.raw() / 4096;
    assert(index && index < nextFrame && frames[index]);
    frames[index] = false;
    --liveFrames;
}
void PmmUnrefPage(PhysicalAddress address) { PmmFreePage(address); }
PhysicalAddress VmmLeafPhysical(uint64_t leaf) { return PhysicalAddress(leaf & ~uint64_t{4095}); }
VmmMapResult VmmTryMapPage(PageTable, VirtualAddress address, PhysicalAddress frame,
                          uint64_t, MemTag, uint16_t, bool) {
    const auto value = address.raw();
    assert(value >= Region && value < Region + RegionBytes && !(value & 4095));
    const auto index = (value - Region) / 4096;
    assert(!mappings[index]);
    if (failMapAfter == 0) return VmmMapResult::OutOfMemory;
    if (failMapAfter > 0) --failMapAfter;
    assert(mprotect(reinterpret_cast<void*>(value), 4096, PROT_READ | PROT_WRITE) == 0);
    mappings[index] = frame.raw() | 1;
    return VmmMapResult::Mapped;
}
uint64_t VmmDetachPage(PageTable, VirtualAddress address) {
    const auto value = address.raw();
    assert(value >= Region && value < Region + RegionBytes && !(value & 4095));
    const auto index = (value - Region) / 4096;
    const auto leaf = mappings[index];
    assert(leaf);
    mappings[index] = 0;
    assert(mprotect(reinterpret_cast<void*>(value), 4096, PROT_NONE) == 0);
    return leaf;
}
extern "C" void SerialPuts(const char* text) { std::fputs(text, stderr); }
extern "C" void SerialPrintf(const char*, ...) {} // kernel format dialect
} // namespace brook

static void fragmented() {
    using namespace brook;
    const auto before = HeapFreeBytes();
    auto* fit = kmalloc(320);
    auto* guard = kmalloc(64);
    assert(fit && guard);
    std::vector<void*> holes, guards;
    for (unsigned i = 0; i < 512; ++i) {
        auto* hole = kmalloc(192);
        auto* live = static_cast<unsigned char*>(kmalloc(64));
        assert(hole && live);
        live[0] = static_cast<unsigned char>(i);
        holes.push_back(hole); guards.push_back(live);
    }
    kfree(fit);
    for (auto* pointer : holes) kfree(pointer);
    assert(HeapCheckIntegrity());
    HeapSetPoison(true);
    assert(HeapCheckIntegrity());
    HeapTimingReset(); HeapTimingEnable(true);
    auto* result = kmalloc(256);
    HeapTimingEnable(false);
    HeapTimingStats timing{}; HeapTimingSnapshot(&timing);
    std::fprintf(stderr, "fragmented-search: headers=%llu expansions=%llu\n",
                 static_cast<unsigned long long>(timing.headers),
                 static_cast<unsigned long long>(timing.expansions));
    assert(result == fit && timing.headers == 1 && !timing.expansions);
    assert(static_cast<unsigned char*>(result)[0] == 0xCD);
    HeapSetPoison(false);
    for (unsigned i = 0; i < 512; ++i) {
        assert(static_cast<unsigned char*>(guards[i])[0] == static_cast<unsigned char>(i));
        kfree(guards[i]);
    }
    kfree(result); kfree(guard);
    assert(HeapCheckIntegrity() && HeapFreeBytes() == before);
}

static void partialBucket() {
    using namespace brook;
    const auto before = HeapFreeBytes();
    auto* a = kmalloc(64); auto* fit = kmalloc(2240);
    auto* b = kmalloc(64); auto* small = kmalloc(2176); auto* c = kmalloc(64);
    assert(a && fit && b && small && c);
    HeapStats stats{}; HeapGetStats(&stats);
    assert(stats.freeBlocks == 1 && stats.largestFreeBlock > 128);
    auto* tail = kmalloc(stats.largestFreeBlock - 128);
    assert(tail);
    kfree(fit); kfree(small);
    HeapTimingReset(); HeapTimingEnable(true);
    auto* result = kmalloc(2240);
    HeapTimingEnable(false);
    HeapTimingStats timing{}; HeapTimingSnapshot(&timing);
    assert(result == fit && timing.headers == 2 && !timing.expansions);
    kfree(result); kfree(a); kfree(b); kfree(c); kfree(tail);
    assert(HeapCheckIntegrity() && HeapFreeBytes() == before);
}

static void failedExpansion() {
    using namespace brook;
    HeapStats before{}; HeapGetStats(&before);
    const auto oldFrames = liveFrames;
    failMapAfter = 2;
    assert(!kmalloc(2 * 1024 * 1024));
    failMapAfter = -1;
    HeapStats after{}; HeapGetStats(&after);
    assert(before.regionEnd == after.regionEnd && before.freeBytes == after.freeBytes);
    assert(liveFrames == oldFrames && HeapCheckIntegrity());
    auto* expanded = kmalloc(2 * 1024 * 1024);
    assert(expanded && liveFrames > oldFrames);
    std::memset(expanded, 0x71, 2 * 1024 * 1024);
    kfree(expanded);
    assert(HeapCheckIntegrity());
}

static void concurrentReuse() {
    using namespace brook;
    const auto before = HeapFreeBytes();
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) workers.emplace_back([worker] {
        for (unsigned iteration = 0; iteration < 200; ++iteration) {
            const unsigned size = 1 + (iteration * 131 + worker * 17) % 4096;
            auto* pointer = static_cast<unsigned char*>(kmalloc(size));
            assert(pointer && !(reinterpret_cast<uint64_t>(pointer) & 63));
            std::memset(pointer, worker + 1, size);
            auto* grown = static_cast<unsigned char*>(krealloc(pointer, size + 137));
            assert(grown);
            for (unsigned byte = 0; byte < size; ++byte) assert(grown[byte] == worker + 1);
            kfree(grown);
        }
    });
    for (auto& worker : workers) worker.join();
    assert(HeapCheckIntegrity() && HeapFreeBytes() == before);
}

int main() {
    void* reserved = mmap(reinterpret_cast<void*>(Region), RegionBytes, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(reserved == reinterpret_cast<void*>(Region));
    brook::HeapInit();
    fragmented(); partialBucket(); failedExpansion(); concurrentReuse();
    const auto before = brook::HeapFreeBytes();
    assert(!brook::kmalloc(~uint64_t{0}) && !brook::kmalloc(uint64_t{1} << 32));
    assert(brook::HeapFreeBytes() == before && brook::HeapCheckIntegrity());
    assert(munmap(reserved, RegionBytes) == 0);
    std::puts("HEAP_INDEX_PASS fragmented/partial-fit/poison/rollback/concurrent-realloc/bounds");
}
