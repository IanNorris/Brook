// test_loopback — Host-native unit tests for the loopback delivery trampoline.
//
// Verifies the BRO-163 invariant: LoopbackSubmit() turns a recursive
// send->deliver->send chain into iteration, so at most ONE deliver() frame is
// ever live on the stack regardless of how many frames bounce. Also covers
// FIFO ordering and bounded drop-on-full behaviour.
//
// No kernel, no networking — exercises loopback_queue.h in isolation.

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#include "loopback_queue.h"

using namespace brook;

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(expr)                                       \
    do {                                                        \
        if (!(expr)) {                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                    __FILE__, __LINE__, #expr);                 \
            g_fail++;                                           \
        } else { g_pass++; }                                    \
    } while (0)

#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        long _a = (long)(a); long _b = (long)(b);               \
        if (_a != _b) {                                         \
            fprintf(stderr, "FAIL %s:%d: %s != %s (%ld != %ld)\n", \
                    __FILE__, __LINE__, #a, #b, _a, _b);        \
            g_fail++;                                           \
        } else { g_pass++; }                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Test harness state — models the kernel's shared queue + drain guard.
// ---------------------------------------------------------------------------

using Queue = LoopbackQueueT<16, 32>;

static Queue        g_q;
static bool         g_draining = false;

// Instrumentation for the recursion invariant.
static int          g_depth     = 0;   // current live deliver() frames
static int          g_maxDepth  = 0;   // high-water of g_depth
static int          g_delivered = 0;   // total deliver() calls
static std::vector<uint32_t> g_order;  // ids in delivery order

// Behaviour knobs, consumed inside deliver().
static int          g_chainRemaining = 0;   // submit one child per deliver
static int          g_fanout         = 0;   // submit N children on first deliver only
static bool         g_fanoutDone     = false;

static void ResetState() {
    g_q.Reset();
    g_draining = false;
    g_depth = g_maxDepth = g_delivered = 0;
    g_order.clear();
    g_chainRemaining = 0;
    g_fanout = 0;
    g_fanoutDone = false;
}

static void Submit(uint32_t id);  // fwd

static void Deliver(const uint8_t* data, uint32_t len) {
    ASSERT_EQ(len, 4u);
    uint32_t id;
    memcpy(&id, data, 4);

    g_depth++;
    if (g_depth > g_maxDepth) g_maxDepth = g_depth;
    g_delivered++;
    g_order.push_back(id);

    // Re-entrant production: this is the whole point of the test — a frame
    // emitted *during* delivery must be queued, not recursed.
    if (g_chainRemaining > 0) {
        g_chainRemaining--;
        Submit(id + 1);
    }
    if (g_fanout > 0 && !g_fanoutDone) {
        g_fanoutDone = true;
        int n = g_fanout;
        for (int i = 0; i < n; i++) Submit(1000 + i);
    }

    g_depth--;
}

static void Submit(uint32_t id) {
    uint8_t frame[4];
    memcpy(frame, &id, 4);
    LoopbackSubmit(g_q, g_draining, frame, 4,
                   []() {}, []() {}, Deliver);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// A long re-entrant chain must deliver iteratively: depth never exceeds 1.
static void TestChainNoRecursion() {
    ResetState();
    g_chainRemaining = 500;     // each deliver spawns the next
    Submit(0);                  // top-level kick

    ASSERT_EQ(g_maxDepth, 1);            // <-- the BRO-163 invariant
    ASSERT_EQ(g_delivered, 501);        // 1 + 500 chained
    ASSERT_EQ(g_q.count, 0u);           // fully drained
    ASSERT_EQ(g_q.dropped, 0u);
    // Ids delivered strictly in order 0,1,2,...,500
    ASSERT_EQ(g_order.size(), 501u);
    bool ordered = true;
    for (uint32_t i = 0; i < g_order.size(); i++)
        if (g_order[i] != i) ordered = false;
    ASSERT_TRUE(ordered);
}

// Even a very deep chain stays O(1) on the stack (would #DF if recursive).
static void TestDeepChainBounded() {
    ResetState();
    g_chainRemaining = 100000;
    Submit(0);
    ASSERT_EQ(g_maxDepth, 1);
    ASSERT_EQ(g_delivered, 100001);
    ASSERT_EQ(g_q.count, 0u);
}

// Frames queued during one deliver drain in FIFO order, still at depth 1.
static void TestFifoFanout() {
    ResetState();
    g_fanout = 5;               // first deliver enqueues 5 children
    Submit(0);

    ASSERT_EQ(g_maxDepth, 1);
    ASSERT_EQ(g_delivered, 6);          // parent + 5
    // Order: parent (0) then children 1000..1004 in submission order.
    ASSERT_EQ(g_order.size(), 6u);
    ASSERT_EQ(g_order[0], 0u);
    bool ordered = true;
    for (int i = 0; i < 5; i++)
        if (g_order[1 + i] != (uint32_t)(1000 + i)) ordered = false;
    ASSERT_TRUE(ordered);
}

// Over-production within a single deliver drops past capacity, never crashes.
static void TestDropOnFull() {
    ResetState();
    // Queue capacity is 16. Fan out 40 children during the first deliver:
    // they accumulate (the loop hasn't drained them yet), so 16 fit and the
    // rest are dropped. No recursion, no overflow.
    g_fanout = 40;
    Submit(0);

    ASSERT_EQ(g_maxDepth, 1);
    ASSERT_EQ(g_q.dropped, (uint64_t)(40 - 16));
    ASSERT_EQ(g_delivered, 1 + 16);     // parent + capacity
    ASSERT_EQ(g_q.count, 0u);
}

// Raw queue FIFO/drop sanity, independent of the drain orchestration.
static void TestQueuePrimitive() {
    Queue q; q.Reset();
    for (uint32_t i = 0; i < Queue::kSlotCount; i++) {
        uint8_t f[4]; memcpy(f, &i, 4);
        ASSERT_TRUE(q.Enqueue(f, 4));
    }
    uint32_t over = 999; uint8_t fo[4]; memcpy(fo, &over, 4);
    ASSERT_TRUE(!q.Enqueue(fo, 4));     // full
    ASSERT_EQ(q.dropped, 1u);
    ASSERT_EQ(q.highWater, Queue::kSlotCount);

    for (uint32_t i = 0; i < Queue::kSlotCount; i++) {
        uint8_t out[32]; uint32_t ol = 0;
        ASSERT_TRUE(q.Dequeue(out, &ol));
        ASSERT_EQ(ol, 4u);
        uint32_t id; memcpy(&id, out, 4);
        ASSERT_EQ(id, i);               // FIFO
    }
    uint8_t out[32]; uint32_t ol = 0;
    ASSERT_TRUE(!q.Dequeue(out, &ol));  // empty

    // Reject zero-length and oversize frames.
    uint8_t big[64] = {0};
    ASSERT_TRUE(!q.Enqueue(big, 64));   // > kSlotSize
    ASSERT_TRUE(!q.Enqueue(big, 0));    // zero len
}

int main() {
    TestQueuePrimitive();
    TestChainNoRecursion();
    TestDeepChainBounded();
    TestFifoFanout();
    TestDropOnFull();

    printf("test_loopback: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
