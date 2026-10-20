#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>
#include <thread>
#include "memory/mapping_ranges.h"

using namespace brook;
namespace {
bool mutation = false;
int allocations = 0, failAfter = -1;
std::set<FileMappingRegistration*> registrations;
}
namespace brook {
extern "C" void* kmalloc(uint64_t bytes) {
    assert(!mutation);
    if (failAfter == 0) return nullptr;
    if (failAfter > 0) --failAfter;
    auto* result = std::malloc(bytes);
    if (result) ++allocations;
    return result;
}
extern "C" void kfree(void* pointer) {
    assert(!mutation);
    if (pointer) { --allocations; std::free(pointer); }
}
void FileMappingRegister(FileMappingRegistration* registration, AddressSpaceState* space,
                         MappingBacking* backing) {
    if (!space || !backing || !backing->file) return;
    assert(registrations.insert(registration).second);
    registration->space = space; registration->file = backing->file;
}
void FileMappingRetire(FileMappingRegistration* registration) {
    if (registration->file) {
        assert(registrations.count(registration)); registration->retiring = true;
    }
}
void FileMappingUnregister(FileMappingRegistration* registration) {
    assert(!mutation);
    if (registration->file) assert(registrations.erase(registration) == 1);
}
}
extern "C" [[noreturn]] void KernelPanic(const char* message, ...) {
    std::fprintf(stderr, "PANIC: %s\n", message); std::abort();
}

static unsigned verifyTree(const MappingRangeNode* node, uint64_t low, uint64_t high,
                           std::set<const MappingRangeNode*>& seen,
                           std::vector<const MappingRangeNode*>& ordered) {
    if (!node) return 0;
    assert(seen.insert(node).second);
    assert(node->range.begin >= low && node->range.begin < high);
    auto left = verifyTree(node->left, low, node->range.begin, seen, ordered);
    ordered.push_back(node);
    auto right = verifyTree(node->right, node->range.begin + 1, high, seen, ordered);
    assert(left <= right + 1 && right <= left + 1);
    assert(node->height == 1 + std::max(left, right));
    return node->height;
}
static unsigned verify(const MappingRangeTable& table) {
    std::set<const MappingRangeNode*> seen;
    std::vector<const MappingRangeNode*> indexed, listed;
    auto height = verifyTree(table.root, 0, 0x800000000000ULL, seen, indexed);
    uint64_t previousEnd = 0;
    for (auto* node = table.head; node; node = node->next) {
        assert(MappingRangeValid(node->range) && previousEnd <= node->range.begin);
        listed.push_back(node); previousEnd = node->range.end;
    }
    assert(indexed == listed);
    for (const auto* cached : table.lookup) {
        if (!cached) continue;
        assert(std::any_of(listed.begin(), listed.end(),
                           [&](auto* node) { return &node->range == cached; }));
    }
    return height;
}
static const MappingRange* oracle(const MappingRangeTable& table, uint64_t address) {
    for (auto* node = table.head; node && node->range.begin <= address; node = node->next)
        if (address < node->range.end) return &node->range;
    return nullptr;
}
static void checkLookup(const MappingRangeTable& table, uint64_t address) {
    auto* expected = oracle(table, address);
    failAfter = 0; mutation = true;
    assert(MappingRangeFind(table, address) == expected);
    mutation = false; failAfter = -1;
}
static MappingRange anonymous(uint64_t begin, uint64_t end, unsigned protection = 3) {
    return {begin, end, 0, 0, nullptr, protection, 7, MappingKind::Anonymous,
            MappingSharing::Private, MappingLeafOwnership::Managed};
}
static void orderedAndDeletion() {
    std::mt19937 random(0x62726f6f);
    for (unsigned order = 0; order < 3; ++order) {
        MappingRangeTable table{};
        std::vector<unsigned> keys(1024);
        for (unsigned i = 0; i < keys.size(); ++i) keys[i] = i;
        if (order == 1) std::reverse(keys.begin(), keys.end());
        if (order == 2) std::shuffle(keys.begin(), keys.end(), random);
        for (auto key : keys) {
            MappingRangePlan plan; assert(plan.Prepare(1));
            auto range = anonymous(0x10000 + key * 8192, 0x11000 + key * 8192);
            mutation = true; assert(plan.Insert(table, range)); mutation = false;
            assert(verify(table) <= 15);
        }
        for (unsigned i = 0; i < 16384; ++i)
            checkLookup(table, 0x10000 + (random() % 2048) * 4096);
        std::shuffle(keys.begin(), keys.end(), random);
        for (auto key : keys) {
            MappingRangePlan plan; assert(plan.Prepare(0));
            mutation = true;
            assert(plan.Replace(table, 0x10000 + key * 8192, 0x11000 + key * 8192));
            mutation = false;
            verify(table); checkLookup(table, 0x10000 + key * 8192);
            // The detached node is retained until Finish, but never indexed.
            plan.Finish(); verify(table);
        }
        assert(!table.head && !table.root);
        MappingRangeDestroy(table); assert(!allocations);
    }
}
static void mixedEdits() {
    constexpr unsigned Pages = 256;
    constexpr uint64_t Base = 0x400000;
    std::vector<int> model(Pages, -1);
    MappingRangeTable table{};
    std::mt19937 random(0x62696e73);
    for (unsigned iteration = 0; iteration < 2500; ++iteration) {
        unsigned first = random() % Pages, count = 1 + random() % 16;
        count = std::min(count, Pages - first);
        uint64_t begin = Base + first * 4096, end = begin + count * 4096;
        MappingRangePlan plan; assert(plan.Prepare(3));
        unsigned operation = random() % 3, protection = random() % 8;
        if (operation == 2) {
            bool covered = true;
            for (unsigned i = first; i < first + count; ++i) covered &= model[i] >= 0;
            mutation = true;
            assert(plan.Protect(table, begin, end, protection) == covered);
            mutation = false;
            if (covered) for (unsigned i = first; i < first + count; ++i) model[i] = protection;
        } else {
            auto range = anonymous(begin, end, protection);
            mutation = true;
            assert(plan.Replace(table, begin, end, operation ? &range : nullptr));
            mutation = false;
            for (unsigned i = first; i < first + count; ++i) model[i] = operation ? int(protection) : -1;
        }
        verify(table);
        for (unsigned i = 0; i < Pages; ++i) {
            auto address = Base + i * 4096 + 4095;
            checkLookup(table, address);
            auto* range = MappingRangeFind(table, address);
            assert(bool(range) == (model[i] >= 0));
            if (range) assert(range->protection == unsigned(model[i]));
        }
        plan.Finish(); verify(table);
    }
    auto* root = table.root; auto identity = table.nextIdentity;
    {
        MappingRangePlan failed;
        failAfter = 2; assert(!failed.Prepare(3)); failAfter = -1;
        assert(table.root == root && table.nextIdentity == identity);
    }
    verify(table);
    MappingRangeDestroy(table); assert(!table.root && !table.head && !allocations);
}
static void backingAndExtend() {
    int released = 0;
    auto* backing = MappingBackingCreate(&released, [](void* object) {
        assert(!mutation); ++*static_cast<int*>(object);
    });
    assert(backing);
    backing->file = reinterpret_cast<FileObject*>(&released); // Identity only; never dereferenced.
    MappingRangeTable table{}; table.owner = reinterpret_cast<AddressSpaceState*>(&table);
    {
        MappingRangePlan plan; assert(plan.Prepare(1));
        auto range = anonymous(0x10000, 0x20000); range.backing = backing;
        mutation = true; assert(plan.Insert(table, range)); mutation = false;
    }
    {
        MappingRangePlan plan; assert(plan.Prepare(2));
        mutation = true; assert(plan.Protect(table, 0x14000, 0x18000, 1)); mutation = false;
    }
    assert(registrations.size() == 3); verify(table);
    auto* last = MappingRangeFind(table, 0x1ffff); assert(last);
    {
        MappingRangePlan plan; assert(plan.Prepare(0));
        mutation = true; assert(plan.Extend(table, last->identity, 0x21000)); mutation = false;
        checkLookup(table, 0x20fff); verify(table);
    }
    {
        MappingRangePlan plan; assert(plan.Prepare(0));
        mutation = true; assert(plan.Replace(table, 0x10000, 0x21000)); mutation = false;
        assert(registrations.size() == 3);
        for (auto* registration : registrations) assert(registration->retiring);
        assert(!MappingRangeFind(table, 0x15000) && !table.root);
    }
    assert(registrations.empty() && !released);
    MappingRangeDestroy(table); MappingBackingUnref(backing);
    assert(released == 1 && !allocations);
}
static void backedTransplant() {
    int released = 0;
    auto* backing = MappingBackingCreate(&released, [](void* object) {
        assert(!mutation); ++*static_cast<int*>(object);
    });
    assert(backing); backing->file = reinterpret_cast<FileObject*>(&released);
    MappingRangeTable table{}; table.owner = reinterpret_cast<AddressSpaceState*>(&table);
    for (unsigned i = 0; i < 7; ++i) {
        MappingRangePlan plan; assert(plan.Prepare(1));
        auto range = anonymous(0x10000 + i * 8192, 0x11000 + i * 8192);
        range.backing = backing;
        mutation = true; assert(plan.Insert(table, range)); mutation = false;
    }
    auto* victim = table.root;
    assert(victim && victim->left && victim->right);
    std::vector<MappingRangeNode*> survivors;
    for (auto* node = table.head; node; node = node->next) if (node != victim) survivors.push_back(node);
    {
        MappingRangePlan plan; assert(plan.Prepare(0));
        mutation = true;
        assert(plan.Replace(table, victim->range.begin, victim->range.end));
        mutation = false;
        assert(registrations.size() == 7 && victim->fileRegistration.retiring);
        for (auto* node : survivors) {
            checkLookup(table, node->range.begin);
            assert(MappingRangeFind(table, node->range.begin) == &node->range);
            assert(registrations.count(&node->fileRegistration) && !node->fileRegistration.retiring);
        }
        verify(table);
    }
    assert(registrations.size() == 6);
    MappingRangeDestroy(table); MappingBackingUnref(backing);
    assert(registrations.empty() && released == 1 && !allocations);
}
static void failedEditsAndReuse() {
    MappingRangeTable table{};
    for (unsigned i = 0; i < 3; ++i) {
        MappingRangePlan plan; assert(plan.Prepare(1));
        auto range = anonymous(0x10000 + i * 8192, 0x11000 + i * 8192);
        mutation = true; assert(plan.Insert(table, range)); mutation = false;
    }
    auto* middle = MappingRangeFind(table, 0x12000); assert(middle);
    {
        MappingRangePlan plan; assert(plan.Prepare(2));
        mutation = true; assert(plan.Extend(table, middle->identity, 0x14000)); mutation = false;
        checkLookup(table, 0x13fff); checkLookup(table, 0x14000);
        auto identity = table.nextIdentity;
        mutation = true;
        assert(!plan.Extend(table, middle->identity, 0x15000));
        assert(!plan.Protect(table, 0x10000, 0x15000, 1)); // Hole after first range.
        assert(!plan.Replace(table, 0x10001, 0x11000));
        mutation = false;
        assert(table.nextIdentity == identity);
        checkLookup(table, 0x13fff); checkLookup(table, 0x14000); verify(table);
    }
    MappingRangeDestroy(table); assert(!allocations);
    {
        MappingRangePlan plan; assert(plan.Prepare(1));
        auto range = anonymous(0x90000, 0x91000);
        mutation = true; assert(plan.Insert(table, range)); mutation = false;
        checkLookup(table, 0x90000); checkLookup(table, 0x12000); verify(table);
    }
    MappingRangeDestroy(table); assert(!allocations);
}
static void concurrentLookupCache() {
    MappingRangeTable table{};
    for(unsigned i=1;i<=16;++i){
        MappingRangePlan plan;assert(plan.Prepare(1));
        auto range=anonymous(uint64_t(i)<<24,(uint64_t(i)<<24)+4096);
        mutation=true;assert(plan.Insert(table,range));mutation=false;
    }
    std::vector<std::thread> readers;
    for(unsigned worker=0;worker<8;++worker)readers.emplace_back([&,worker]{
        for(unsigned n=0;n<20000;++n){
            const uint64_t begin=uint64_t(1+(n*7+worker)%16)<<24;
            const auto* range=MappingRangeFind(table,begin+17);
            assert(range && range->begin==begin && range->end==begin+4096);
        }
    });
    for(auto& reader:readers)reader.join();
    MappingRangeDestroy(table);assert(!allocations);
}
int main() {
    concurrentLookupCache();
    orderedAndDeletion(); mixedEdits(); backingAndExtend(); backedTransplant(); failedEditsAndReuse();
    std::puts("MAPPING_RANGE_INDEX_PASS ordered/random rotations, edits, lookup oracle, OOM, backing lifetime");
}
