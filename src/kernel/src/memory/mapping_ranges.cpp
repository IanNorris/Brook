#include "mapping_ranges.h"
#include "heap.h"
#include "panic.h"

namespace brook {
static constexpr uint64_t UserEnd = 0x0000800000000000ULL;
static void InvalidateLookup(MappingRangeTable& table) {
    for (auto& entry : table.lookup) entry = nullptr;
}
static uint32_t Height(const MappingRangeNode* node) { return node ? node->height : 0; }
static void UpdateHeight(MappingRangeNode* node) {
    auto left = Height(node->left), right = Height(node->right);
    node->height = 1 + (left > right ? left : right);
}
static MappingRangeNode* RotateLeft(MappingRangeNode* node) {
    auto* top = node->right;
    node->right = top->left; top->left = node;
    UpdateHeight(node); UpdateHeight(top); return top;
}
static MappingRangeNode* RotateRight(MappingRangeNode* node) {
    auto* top = node->left;
    node->left = top->right; top->right = node;
    UpdateHeight(node); UpdateHeight(top); return top;
}
static MappingRangeNode* Balance(MappingRangeNode* node) {
    if (!node) return nullptr;
    UpdateHeight(node);
    if (Height(node->left) > Height(node->right) + 1) {
        if (Height(node->left->right) > Height(node->left->left))
            node->left = RotateLeft(node->left);
        return RotateRight(node);
    }
    if (Height(node->right) > Height(node->left) + 1) {
        if (Height(node->right->left) > Height(node->right->right))
            node->right = RotateRight(node->right);
        return RotateLeft(node);
    }
    return node;
}
static MappingRangeNode* IndexInsert(MappingRangeNode* root, MappingRangeNode* node) {
    if (!root) return node;
    if (node->range.begin < root->range.begin) root->left = IndexInsert(root->left, node);
    else if (node->range.begin > root->range.begin) root->right = IndexInsert(root->right, node);
    else KernelPanic("mapping: duplicate range index key");
    return Balance(root);
}
static MappingRangeNode* IndexRemove(MappingRangeNode* root, MappingRangeNode* node) {
    if (!root) KernelPanic("mapping: missing range index key");
    if (node->range.begin < root->range.begin) root->left = IndexRemove(root->left, node);
    else if (node->range.begin > root->range.begin) root->right = IndexRemove(root->right, node);
    else {
        if (root != node) KernelPanic("mapping: mismatched range index identity");
        if (!root->left) return root->right;
        if (!root->right) return root->left;
        // Transplant the successor node itself. Copying its payload would
        // invalidate range pointers and embedded file-registry identities.
        auto* successor = root->right;
        while (successor->left) successor = successor->left;
        auto* right = IndexRemove(root->right, successor);
        successor->left = root->left; successor->right = right;
        return Balance(successor);
    }
    return Balance(root);
}
static void IndexErase(MappingRangeTable& table, MappingRangeNode* node) {
    table.root = IndexRemove(table.root, node);
    node->left = node->right = nullptr; node->height = 1;
}
static bool ValidRange(uint64_t begin, uint64_t end) {
    return begin < end && end <= UserEnd && !(begin & 4095) && !(end & 4095);
}
bool MappingRangeValid(const MappingRange& range) {
    return ValidRange(range.begin, range.end) && !(range.offset & 4095) &&
           range.offset <= ~uint64_t{0} - (range.end - range.begin) &&
           !(range.maximumProtection & ~7u) && !(range.protection & ~range.maximumProtection) &&
           (range.leafOwnership != MappingLeafOwnership::Borrowed || range.backing) &&
           ((range.kind != MappingKind::File && range.kind != MappingKind::MemFd &&
             range.kind != MappingKind::Device) || range.backing);
}
MappingBacking* MappingBackingCreate(void* object, void (*release)(void*)) {
    if (!object || !release) return nullptr;
    auto* backing = static_cast<MappingBacking*>(kmalloc(sizeof(MappingBacking)));
    if (backing) *backing = {1, object, release};
    return backing;
}
void MappingBackingRef(MappingBacking* backing) {
    if (backing && !__atomic_fetch_add(&backing->references, 1, __ATOMIC_ACQ_REL))
        KernelPanic("mapping: retaining retired backing");
}
void MappingBackingUnref(MappingBacking* backing) {
    if (!backing) return;
    auto before = __atomic_fetch_sub(&backing->references, 1, __ATOMIC_ACQ_REL);
    if (!before) KernelPanic("mapping: backing reference underflow");
    if (before == 1) { backing->release(backing->object); kfree(backing); }
}
static void ReleaseNodes(MappingRangeNode* node) {
    while (node) {
        auto* next = node->next;
        FileMappingUnregister(&node->fileRegistration);
        MappingBackingUnref(node->range.backing); kfree(node); node = next;
    }
}
MappingRangePlan::~MappingRangePlan() { Finish(); }
void MappingRangePlan::Finish() {
    auto* spare = spare_; auto* retired = retired_;
    spare_ = retired_ = nullptr; available_ = 0;
    ReleaseNodes(spare); ReleaseNodes(retired);
}
bool MappingRangePlan::Prepare(uint64_t nodes) {
    if (prepared_) KernelPanic("mapping: repeated range preparation");
    prepared_ = true;
    for (uint64_t i = 0; i < nodes; ++i) {
        auto* node = static_cast<MappingRangeNode*>(kmalloc(sizeof(MappingRangeNode)));
        if (!node) return false;
        *node = {{}, spare_, {}}; spare_ = node; ++available_;
    }
    return true;
}
MappingRangeNode* MappingRangePlan::Take(const MappingRange& source, uint64_t begin,
                                        uint64_t end, MappingRangeTable& table) {
    auto* node = spare_; spare_ = node->next; --available_;
    node->range = source; node->range.offset += begin - source.begin;
    node->range.begin = begin; node->range.end = end;
    node->range.identity = ++table.nextIdentity; node->next = nullptr;
    MappingBackingRef(node->range.backing);
    FileMappingRegister(&node->fileRegistration,table.owner,node->range.backing);
    return node;
}
bool MappingRangePlan::Replace(MappingRangeTable& table, uint64_t begin, uint64_t end,
                               const MappingRange* replacement) {
    if (!CanReplace(table,begin,end,replacement)) return false;
    InvalidateLookup(table);
    MappingRangeNode** first = &table.head;
    while (*first && (*first)->range.end <= begin) first = &(*first)->next;
    auto* stop = *first;
    const MappingRange* left = nullptr; const MappingRange* right = nullptr;
    while (stop && stop->range.begin < end) {
        if (stop->range.begin < begin) left = &stop->range;
        if (stop->range.end > end) right = &stop->range;
        stop = stop->next;
    }
    // Build the new chain before changing links: boundary descriptors can live
    // in the same old node, and all backing references must survive retirement.
    MappingRangeNode* chain = nullptr; MappingRangeNode** tail = &chain;
    auto append = [&](MappingRangeNode* node) { *tail = node; tail = &node->next; };
    if (left) append(Take(*left,left->begin,begin,table));
    if (replacement) append(Take(*replacement,begin,end,table));
    if (right) append(Take(*right,end,right->end,table));
    auto* old = *first;
    while (old != stop) {
        IndexErase(table, old);
        FileMappingRetire(&old->fileRegistration);
        auto* next = old->next; old->next = retired_; retired_ = old; old = next;
    }
    for (auto* node = chain; node; node = node->next)
        table.root = IndexInsert(table.root, node);
    *tail = stop; *first = chain ? chain : stop;
    return true;
}
bool MappingRangePlan::CanReplace(const MappingRangeTable& table, uint64_t begin, uint64_t end,
                                  const MappingRange* replacement) const {
    if (!prepared_ || !ValidRange(begin,end) || (replacement &&
        (!MappingRangeValid(*replacement) || replacement->begin != begin || replacement->end != end))) return false;
    uint64_t needed = replacement != nullptr;
    for (auto* node = table.head; node && node->range.begin < end; node = node->next) {
        if (node->range.end <= begin) continue;
        needed += (node->range.begin < begin) + (node->range.end > end);
    }
    return available_ >= needed && table.nextIdentity <= ~uint64_t{0} - needed;
}
bool MappingRangePlan::Insert(MappingRangeTable& table, const MappingRange& range) {
    if (!MappingRangeValid(range)) return false;
    for (auto* node = table.head; node && node->range.begin < range.end; node = node->next)
        if (node->range.end > range.begin) return false;
    return Replace(table,range.begin,range.end,&range);
}
bool MappingRangePlan::CanProtect(const MappingRangeTable& table, uint64_t begin, uint64_t end,
                                  uint32_t protection) const {
    if (!prepared_ || !ValidRange(begin,end) || (protection & ~7u)) return false;
    auto* first = table.head;
    while (first && first->range.end <= begin) first = first->next;
    uint64_t covered = begin, splits = 0, changed = 0;
    for (auto* node = first; node && node->range.begin < end; node = node->next) {
        const auto& range = node->range;
        if (range.begin > covered || (protection & ~range.maximumProtection)) return false;
        if (range.begin < begin) ++splits;
        if (range.end > end) ++splits;
        covered = range.end; ++changed;
    }
    if (covered < end || available_ < splits ||
        table.nextIdentity > ~uint64_t{0} - splits - changed) return false;
    return true;
}
bool MappingRangePlan::Protect(MappingRangeTable& table, uint64_t begin, uint64_t end,
                               uint32_t protection) {
    if (!CanProtect(table,begin,end,protection)) return false;
    InvalidateLookup(table);
    MappingRangeNode** link = &table.head;
    while (*link && (*link)->range.end <= begin) link = &(*link)->next;
    while (*link && (*link)->range.begin < end) {
        auto* node = *link; const auto original = node->range;
        IndexErase(table, node);
        if (original.begin < begin) {
            auto* left = Take(original,original.begin,begin,table);
            table.root = IndexInsert(table.root, left);
            left->next = node; *link = left; link = &left->next;
            node->range.begin = begin; node->range.offset += begin - original.begin;
        }
        if (original.end > end) {
            auto* right = Take(original,end,original.end,table);
            table.root = IndexInsert(table.root, right);
            right->next = node->next; node->next = right; node->range.end = end;
        }
        node->range.protection = protection; node->range.identity = ++table.nextIdentity;
        table.root = IndexInsert(table.root, node);
        link = &node->next;
    }
    return true;
}
const MappingRange* MappingRangeFind(const MappingRangeTable& table, uint64_t address) {
    const uint64_t page = address >> 12;
    const unsigned slot = (page ^ (page >> 8) ^ (page >> 16)) & (MappingRangeTable::LookupSlots-1);
    if (const auto* cached = table.lookup[slot])
        if (cached->begin <= address && address < cached->end) return cached;
    for (auto* node = table.root; node;) {
        if (address < node->range.begin) node = node->left;
        else if (address >= node->range.end) node = node->right;
        else {
            table.lookup[slot] = &node->range;
            return &node->range;
        }
    }
    return nullptr;
}
bool MappingRangePlan::Extend(MappingRangeTable& table,uint64_t identity,uint64_t end) {
    if (!prepared_) return false;
    for (auto* node=table.head;node;node=node->next) {
        if (node->range.identity!=identity) continue;
        auto extended=node->range;extended.end=end;
        if (end<node->range.end || !MappingRangeValid(extended) ||
            (node->next && end>node->next->range.begin) || table.nextIdentity==~uint64_t{0}) return false;
        InvalidateLookup(table);
        extended.identity=++table.nextIdentity;node->range=extended;return true;
    }
    return false;
}
uint64_t MappingRangeFindGap(const MappingRangeTable& table, uint64_t hint,
                            uint64_t length, uint64_t floor, uint64_t ceiling) {
    if (!length || (length & 4095) || (floor & 4095) || (ceiling & 4095) ||
        floor >= ceiling || ceiling > UserEnd || length > ceiling-floor) return 0;
    uint64_t start = hint >= floor && hint < ceiling ? hint : floor;
    if (start > ~uint64_t{0}-4095) return 0;
    start = (start + 4095) & ~uint64_t{4095};
    for (auto* node = table.head; node; node = node->next) {
        if (node->range.end <= start) continue;
        if (node->range.begin >= start && node->range.begin-start >= length) break;
        start = node->range.end;
        if (start > ceiling || length > ceiling-start) return 0;
    }
    return start <= ceiling && length <= ceiling-start ? start : 0;
}
void MappingRangeDestroy(MappingRangeTable& table) {
    InvalidateLookup(table);
    auto* nodes = table.head; table.head = table.root = nullptr; ReleaseNodes(nodes);
}
}
