// test_tcp_receive_fuzz — deterministic fuzz of TCP receive reassembly.
//
// Drives the kernel's receive path (tcp_receive.h + TcpProcessSegment) with a
// simulated peer that cuts a random byte stream into overlapping segments,
// reorders, duplicates and drops them, retransmits, crosses the 32-bit
// sequence wrap, honours (and sometimes exceeds) the advertised window, and
// changes the receive limit and ring capacity while data is held. The bytes
// read from the socket must equal the stream exactly, and FIN must be seen
// only after the last byte.
//
// Usage: test_tcp_receive_fuzz [seeds [first-seed [fuzz-only]]]

#include "net.h"
#include "tcp.h"
#include "tcp_receive.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace brook;

namespace brook {
volatile uint64_t g_lapicTickCount = 0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
}

static TcpReceiveDiagnostics g_diag{};
static unsigned g_currentSeed = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr, "FAIL seed=%u %s:%d: %s\n", g_currentSeed, __FILE__, \
                    __LINE__, #expr);                                            \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

static SocketRxStorage AllocateRx(uint32_t limit)
{
    uint32_t capacity = 4096;
    while (capacity < limit) capacity *= 2;
    SocketRxStorage storage{new uint8_t[capacity], new uint64_t[capacity / 64], capacity};
    memset(storage.ring, 0xcc, capacity); // garbage: never readable before written
    memset(storage.bitmap, 0, capacity / 8);
    return storage;
}

static void FreeRx(SocketRxStorage storage)
{
    delete[] storage.ring;
    delete[] storage.bitmap;
}

struct Receiver {
    Socket s{};
    uint32_t isn = 0;
    uint64_t delivered = 0; // bytes read by the application

    void Init(uint32_t initialSeq, uint32_t limit, uint8_t scale)
    {
        s = {};
        s.type = SOCK_STREAM;
        s.tcpState = TcpState::Established;
        s.connected = true;
        s.tcpSndNxt = s.tcpSndUna = 1000;
        s.tcpRcvNxt = isn = initialSeq;
        s.tcpRcvWndScale = scale;
        const auto storage = AllocateRx(limit);
        s.rxBuf = storage.ring;
        s.tcpOooMap = storage.bitmap;
        s.rxCapacity = storage.capacity;
        s.rxLimit = limit;
        delivered = 0;
    }
    void Destroy() { FreeRx({s.rxBuf, s.tcpOooMap, s.rxCapacity}); s = {}; }

    // Bytes accepted in order (RCV.NXT minus ISN, excluding FIN).
    uint64_t Accepted() const { return uint32_t(s.tcpRcvNxt - isn) - (s.tcpFinRecv ? 1 : 0); }

    // Mirror CaptureTcpTxLocked: the window that the next ACK carries.
    uint32_t Advertise()
    {
        const uint32_t free = TcpReceiveFree(s);
        const uint16_t window = TcpAdvertisedWindow(free, s.tcpRcvWndScale, 0);
        const uint16_t synWindow = TcpAdvertisedWindow(free, s.tcpRcvWndScale, TCP_SYN);
        TcpNoteReceiveGrant(s, window, synWindow);
        return uint32_t(window) << s.tcpRcvWndScale;
    }

    TcpReceiveResult Deliver(const std::vector<uint8_t>& stream, uint64_t off, uint32_t len, bool fin)
    {
        const uint8_t flags = TCP_ACK | (fin ? TCP_FIN : 0);
        return TcpReceiveSegmentLocked(s, isn + uint32_t(off), s.tcpSndNxt, flags,
                                       len ? stream.data() + off : nullptr, len,
                                       g_lapicTickCount, g_diag);
    }

    // Read like SockRecv and verify against the stream.
    void Read(const std::vector<uint8_t>& stream, uint32_t want)
    {
        static std::vector<uint8_t> buffer;
        buffer.resize(want);
        const uint32_t got = TcpConsumeReceived(s, buffer.data(), want);
        CHECK(delivered + got <= stream.size());
        for (uint32_t i = 0; i < got; ++i) {
            if (buffer[i] != stream[delivered + i]) {
                fprintf(stderr, "FAIL seed=%u: byte %llu read 0x%02x expected 0x%02x\n",
                        g_currentSeed, (unsigned long long)(delivered + i), buffer[i],
                        stream[delivered + i]);
                exit(1);
            }
        }
        delivered += got;
    }

    // Grow the ring as SetSocketRxLimit does (allocate, then install).
    void Grow(uint32_t limit)
    {
        if (limit <= s.rxCapacity) { s.rxLimit = limit; return; }
        const auto old = InstallSocketRxLocked(s, AllocateRx(limit));
        s.rxLimit = limit;
        FreeRx(old);
    }

    // Structural and content invariants of the ring and reassembly bitmap.
    void Verify(const std::vector<uint8_t>& stream, bool full)
    {
        const uint64_t accepted = Accepted();
        CHECK(accepted <= stream.size());
        CHECK(delivered + s.rxCount == accepted);
        CHECK(s.rxCount <= s.rxCapacity);
        CHECK(((s.rxTail + s.rxCount) & (s.rxCapacity - 1)) == s.rxHead);
        if (s.tcpFinRecv) CHECK(accepted == stream.size());
        if (s.tcpFinRecv) CHECK(s.tcpOooBytes == 0);
        // The position of RCV.NXT is never marked after a drain.
        CHECK(!(s.tcpOooMap[s.rxHead / 64] >> (s.rxHead % 64) & 1));
        if (!full) return;
        uint64_t bits = 0;
        for (uint32_t word = 0; word < s.rxCapacity / 64; ++word) {
            uint64_t w = s.tcpOooMap[word];
            while (w) {
                const unsigned bit = __builtin_ctzll(w);
                w &= w - 1;
                ++bits;
                const uint32_t position = word * 64 + bit;
                const uint32_t ahead = (position - s.rxHead) & (s.rxCapacity - 1);
                // Future data never overlaps readable bytes.
                CHECK(ahead < s.rxCapacity - s.rxCount);
                CHECK(accepted + ahead < stream.size());
                CHECK(s.rxBuf[position] == stream[accepted + ahead]);
            }
        }
        CHECK(bits == s.tcpOooBytes);
        for (uint32_t i = 0; i < s.rxCount; ++i)
            CHECK(s.rxBuf[(s.rxTail + i) & (s.rxCapacity - 1)] == stream[delivered + i]);
    }
};

struct Segment {
    uint64_t off;
    uint32_t len;
    bool fin;
};

// One randomized connection. Returns the number of segments delivered.
static uint64_t RunSeed(unsigned seed)
{
    g_currentSeed = seed;
    std::mt19937_64 rng(seed * 0x9e3779b97f4a7c15ull + 1);
    auto below = [&](uint64_t n) { return n ? rng() % n : 0; };
    auto chance = [&](unsigned percent) { return below(100) < percent; };

    // Stream: up to 384 KiB, sometimes tiny.
    const uint64_t size = chance(10) ? 1 + below(200) : 1 + below(384 * 1024);
    std::vector<uint8_t> stream(size);
    for (auto& byte : stream) byte = uint8_t(rng());

    // ISN placed so the stream crosses the 32-bit wrap most of the time.
    const uint32_t isn = chance(80) ? uint32_t(0) - uint32_t(below(size + 1)) : uint32_t(rng());
    static const uint32_t limits[] = {Socket::RX_MIN, 4096, 6000, 16384, 65536, 128 * 1024, 300000};
    const uint32_t limit = limits[below(sizeof(limits) / sizeof(limits[0]))];
    const uint8_t scale = chance(70) ? 7 : uint8_t(below(10));
    static const uint32_t mssChoices[] = {1, 7, 100, 536, 1448, 1460, 4000, 9000};
    const uint32_t mss = mssChoices[below(sizeof(mssChoices) / sizeof(mssChoices[0]))];

    // Per-seed behaviour mix.
    const unsigned dropPct = unsigned(below(30));
    const unsigned dupPct = unsigned(below(30));
    const unsigned overshootPct = unsigned(below(15));
    const unsigned readPct = 5 + unsigned(below(60));
    const unsigned resizePct = chance(50) ? unsigned(below(3)) : 0;

    Receiver r;
    r.Init(isn, limit, scale);
    g_lapicTickCount = 1000;

    uint64_t peerAck = 0;                 // highest cumulative ACK the peer has seen
    uint64_t peerEdge = r.Advertise();    // right edge offset the peer believes
    uint64_t sndNxt = 0;                  // highest offset the peer has sent
    std::vector<Segment> pool;            // segments in flight (reorder buffer)
    uint64_t segments = 0;

    auto makeSegment = [&](uint64_t off, uint64_t maxLen) {
        Segment seg{off, 0, false};
        if (off > size) off = seg.off = size;
        uint64_t len = 1 + below(maxLen ? maxLen : 1);
        if (len > size - off) len = size - off;
        seg.len = uint32_t(len);
        seg.fin = off + len == size && chance(70);
        if (off + len > sndNxt) sndNxt = off + len;
        return seg;
    };
    auto ack = [&]() {
        const uint32_t window = r.Advertise();
        if (chance(10)) return; // ACK lost
        const uint64_t accepted = r.Accepted();
        if (accepted > peerAck) peerAck = accepted;
        peerEdge = accepted + window;
    };
    auto deliver = [&](const Segment& seg) {
        const auto result = r.Deliver(stream, seg.off, seg.len, seg.fin);
        ++segments;
        if (result.act.sendAck || result.bufferWasFull) ack();
    };

    const unsigned steps = 2000 + unsigned(below(30000));
    for (unsigned step = 0; step < steps && !r.s.tcpFinRecv; ++step) {
        const unsigned action = unsigned(below(100));
        if (action < 45) {
            // Peer transmits: next new data, a retransmit from the ACK point,
            // or an arbitrary overlapping cut around the window.
            Segment seg{};
            const unsigned kind = unsigned(below(100));
            uint64_t allowed = peerEdge > sndNxt ? peerEdge - sndNxt : 0;
            if (chance(overshootPct)) allowed += 1 + below(3 * mss);
            if (kind < 50 && sndNxt < size && allowed) {
                seg = makeSegment(sndNxt, allowed < mss ? allowed : mss);
            } else if (kind < 75) {
                seg = makeSegment(peerAck, mss);
            } else {
                const uint64_t base = peerAck > 3000 ? peerAck - 3000 : 0;
                const uint64_t span = (peerEdge > base ? peerEdge - base : 0) + 3000 + 3 * mss;
                seg = makeSegment(base + below(span), 1 + below(3 * mss));
            }
            if (seg.len == 0 && seg.off == size) seg.fin = true; // FIN-only probe
            if (chance(dropPct)) continue;
            pool.push_back(seg);
            if (chance(dupPct)) pool.push_back(seg);
        } else if (action < 80) {
            if (pool.empty()) continue;
            // Mostly near-FIFO with reordering; occasionally a deep reorder.
            const size_t window = chance(80) ? (pool.size() < 4 ? pool.size() : 4) : pool.size();
            const size_t index = below(window);
            const Segment seg = pool[index];
            pool.erase(pool.begin() + long(index));
            deliver(seg);
        } else if (action < 81 + readPct / 4) {
            if (r.s.rxCount) r.Read(stream, 1 + uint32_t(below(r.s.rxCount + 2000)));
            if (chance(30)) ack(); // window update
        } else if (action < 97) {
            if (chance(resizePct)) {
                // SO_RCVBUF shrink (keeps capacity) or automatic growth.
                if (chance(50)) r.Grow(Socket::RX_MIN + uint32_t(below(r.s.rxCapacity - Socket::RX_MIN + 1)));
                else r.Grow(r.s.rxLimit * 2 > 1u << 20 ? 1u << 20 : r.s.rxLimit * 2);
            }
        } else if (action < 98) {
            if (chance(5)) g_lapicTickCount += 31000; // OOO reap after 30 s
            else g_lapicTickCount += below(50);
        } else {
            ack();
        }
        const bool full = r.s.rxCapacity <= 16384 || (step % 64) == 0;
        r.Verify(stream, full);
    }

    // Recovery: a well-behaved peer retransmits from its ACK point. After all
    // data is acknowledged it retransmits the last data segment with FIN,
    // as a sender that does not trim acknowledged data would.
    pool.clear();
    uint64_t lastStart = size > mss ? size - mss : 0;
    for (unsigned round = 0; !r.s.tcpFinRecv || r.s.rxCount; ++round) {
        if (round >= 8 * size + 100000) {
            fprintf(stderr, "stuck: size=%llu accepted=%llu delivered=%llu rxCount=%u limit=%u cap=%u "
                    "free=%u ooo=%u fin=%d mss=%u scale=%u edge=%d\n",
                    (unsigned long long)size, (unsigned long long)r.Accepted(),
                    (unsigned long long)r.delivered, r.s.rxCount, r.s.rxLimit, r.s.rxCapacity,
                    TcpReceiveFree(r.s), r.s.tcpOooBytes, int(r.s.tcpFinRecv), mss,
                    r.s.tcpRcvWndScale, int32_t(r.s.tcpRxRightEdge - r.s.tcpRcvNxt));
            CHECK(!"recovery made no progress");
        }
        if (r.s.rxCount && (chance(70) || r.s.tcpFinRecv)) r.Read(stream, 1 + uint32_t(below(r.s.rxCount + 4096)));
        if (r.s.tcpFinRecv) continue;
        const uint64_t accepted = r.Accepted();
        peerAck = accepted;
        const uint32_t window = r.Advertise();
        Segment seg{};
        if (accepted >= size) {
            seg = {lastStart, uint32_t(size - lastStart), true};
        } else {
            uint64_t len = window < mss ? window : mss;
            if (!len) len = 1; // zero-window probe
            if (len > size - accepted) len = size - accepted;
            seg = {accepted, uint32_t(len), accepted + len == size};
            if (seg.fin) lastStart = seg.off;
        }
        deliver(seg);
        r.Verify(stream, r.s.rxCapacity <= 16384 || (round % 64) == 0);
    }
    CHECK(r.delivered == size);
    CHECK(r.s.tcpFinRecv);
    CHECK(r.s.tcpRcvNxt == isn + uint32_t(size) + 1);
    r.Verify(stream, true);
    r.Destroy();
    return segments;
}

// ---------------------------------------------------------------------------
// Targeted cases: each shape of overlap the fuzz is meant to reach.
// ---------------------------------------------------------------------------

static void Targeted()
{
    g_currentSeed = ~0u;
    std::vector<uint8_t> stream(20000);
    for (size_t i = 0; i < stream.size(); ++i) stream[i] = uint8_t(i * 131 + (i >> 8));
    const uint32_t isn = 0xfffff000u; // wraps after 4096 bytes

    Receiver r;
    auto fresh = [&]() { r.Init(isn, 65536, 7); r.Advertise(); };
    auto readAll = [&]() { r.Read(stream, r.s.rxCount); };

    // Hold [100,200); left overlap [50,150); right overlap [180,260);
    // containment [120,130); exact duplicate [100,200); then fill [0,100).
    fresh();
    r.Deliver(stream, 100, 100, false);
    r.Deliver(stream, 50, 100, false);
    r.Deliver(stream, 180, 80, false);
    r.Deliver(stream, 120, 10, false);
    r.Deliver(stream, 100, 100, false);
    CHECK(r.s.tcpOooBytes == 210);
    r.Verify(stream, true);
    r.Deliver(stream, 0, 100, false);
    CHECK(r.Accepted() == 260 && r.s.tcpOooBytes == 0);
    readAll();
    r.Verify(stream, true);
    r.Destroy();

    // Both-sides overlap: [300,400) and [500,600) held, then [350,550).
    fresh();
    r.Deliver(stream, 300, 100, false);
    r.Deliver(stream, 500, 100, false);
    r.Deliver(stream, 350, 200, false);
    CHECK(r.s.tcpOooBytes == 300);
    r.Deliver(stream, 0, 300, false);
    CHECK(r.Accepted() == 600);
    readAll();
    r.Verify(stream, true);
    r.Destroy();

    // Overlap with already-delivered data, across the sequence wrap.
    fresh();
    r.Deliver(stream, 0, 4000, false);
    readAll();
    r.Deliver(stream, 4200, 300, false);
    r.Deliver(stream, 3900, 400, false); // 100 stale, 300 new, 100 overlaps held
    CHECK(r.Accepted() == 4500 && r.s.tcpOooBytes == 0);
    readAll();
    r.Verify(stream, true);
    r.Destroy();

    // OOO segment carrying FIN is not accepted early; the FIN arrives later
    // on a retransmit whose data is already held, then on a pure duplicate.
    fresh();
    r.Deliver(stream, 1000, 500, true);
    CHECK(!r.s.tcpFinRecv);
    r.Deliver(stream, 0, 1000, false);
    CHECK(r.Accepted() == 1500 && !r.s.tcpFinRecv);
    r.Deliver(stream, 1000, 500, true); // all data duplicate; FIN is new
    CHECK(r.s.tcpFinRecv && r.s.tcpRcvNxt == isn + 1501);
    readAll();
    CHECK(r.delivered == 1500);
    r.Destroy();

    // A retransmit that ends partly inside the data and carries FIN after it.
    fresh();
    r.Deliver(stream, 0, 1450, false);
    r.Deliver(stream, 1400, 100, true);
    CHECK(r.s.tcpFinRecv && r.Accepted() == 1500);
    r.Destroy();

    // Receive window full: new bytes are clamped, FIN stripped, nothing lost.
    r.Init(isn, Socket::RX_MIN, 0);
    r.Advertise();
    r.Deliver(stream, 0, 2000, false);
    const auto clamped = r.Deliver(stream, 2000, 1000, true);
    CHECK(r.Accepted() == Socket::RX_MIN && !r.s.tcpFinRecv);
    r.Deliver(stream, Socket::RX_MIN, 10, false);
    CHECK(clamped.act.sendAck && r.Accepted() == Socket::RX_MIN);
    r.Deliver(stream, 0, 1, false); // stale probe
    readAll();
    r.Advertise();
    r.Deliver(stream, 2000, 1000, true);
    CHECK(r.s.tcpFinRecv && r.Accepted() == 3000);
    readAll();
    CHECK(r.delivered == 3000);
    r.Destroy();

    // Growth while future bytes are held across the ring wrap.
    r.Init(isn, 4096, 7);
    r.Advertise();
    r.Deliver(stream, 0, 3000, false);
    r.Read(stream, 3000);
    r.Advertise();
    r.Deliver(stream, 3500, 3000, false); // crosses the 4 KiB ring end
    r.Grow(65536);
    r.Verify(stream, true);
    r.Deliver(stream, 3000, 500, false);
    CHECK(r.Accepted() == 6500);
    readAll();
    r.Verify(stream, true);
    r.Destroy();
    printf("targeted cases: OK\n");
}

int main(int argc, char** argv)
{
    const unsigned seeds = argc > 1 ? unsigned(strtoul(argv[1], nullptr, 0)) : 200;
    const unsigned first = argc > 2 ? unsigned(strtoul(argv[2], nullptr, 0)) : 1;
    if (argc <= 3) Targeted(); // any third argument: fuzz only
    uint64_t segments = 0;
    for (unsigned seed = first; seed < first + seeds; ++seed) segments += RunSeed(seed);
    printf("fuzz: %u seeds from %u, %llu segments, ooo_stored=%llu ooo_overlap=%llu "
           "ooo_capacity=%llu clipped=%llu: OK\n",
           seeds, first, (unsigned long long)segments, (unsigned long long)g_diag.oooStored,
           (unsigned long long)g_diag.oooOverlap, (unsigned long long)g_diag.oooCapacity,
           (unsigned long long)g_diag.clippedSegments);
    return 0;
}
