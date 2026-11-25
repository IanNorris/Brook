// Host test for nv_gsp_queue.h: the CPU side of the GSP-RM cmdq/msgq,
// against a fake GSP peer thread written independently from the queue code
// (it only shares the struct layouts). Covers ring wrap-around with
// elements straddling the end, CONTINUATION_RECORD split/reassembly in both
// directions, out-of-order events before a reply, RPC errors, checksum
// corruption, timeouts and a foreign record where a continuation is due.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include "nv_gsp/nv_gsp_queue.h"

using namespace brook::nvgsp;
namespace rm = brook::nvgsp::rm570;

namespace {

uint32_t A32(const uint8_t* p) { return __atomic_load_n(reinterpret_cast<const uint32_t*>(p), __ATOMIC_ACQUIRE); }
void A32w(uint8_t* p, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(p), v, __ATOMIC_RELEASE); }
uint32_t R32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void W32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

struct Budget {
    std::chrono::steady_clock::time_point end;
    explicit Budget(int ms = 5000) : end(std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)) {}
    bool operator()() { std::this_thread::yield(); return std::chrono::steady_clock::now() < end; }
};
struct Bell { int rings = 0; void operator()() { ++rings; } };

// ------------------------------------------------------------ fake GSP
// Owns msgq page 0 (tx header) and msgq.rx.readPtr (its cmdq read pointer).
class FakeGsp {
public:
    FakeGsp(const QueueShm& q) : cmdq_(q.cmdq()), msgq_(q.msgq()) {
        rm::msgqTxHeader tx = {};
        tx.size = 0x40000; tx.msgSize = 0x1000; tx.msgCount = 63; tx.flags = 1; tx.rxHdrOff = 32; tx.entryOff = 0x1000;
        std::memcpy(msgq_, &tx, sizeof(tx));
    }
    uint8_t* Slot(uint8_t* q, uint32_t i) { return q + 0x1000 + i * 0x1000; }

    // Read one element from cmdq; returns the rpc bytes (header + payload).
    bool ReadElement(std::vector<uint8_t>* rpc, Budget& wait) {
        const uint32_t r = A32(msgq_ + 32);
        while (A32(cmdq_ + 16) == r) if (!wait()) return false;
        std::vector<uint8_t> first(0x1000);
        std::memcpy(first.data(), Slot(cmdq_, r), 0x1000);
        const uint32_t count = R32(first.data() + 40);
        assert(count >= 1 && count <= 16);
        while ((A32(cmdq_ + 16) + 63 - r) % 63 < count) if (!wait()) return false;
        std::vector<uint8_t> e(count * 0x1000);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t s = (r + i) % 63;
            if (s < r) straddled_cmdq = true;
            std::memcpy(e.data() + i * 0x1000, Slot(cmdq_, s), 0x1000);
        }
        uint64_t c = 0, w;
        for (size_t i = 0; i < e.size(); i += 8) { std::memcpy(&w, e.data() + i, 8); c ^= w; }
        assert((uint32_t(c >> 32) ^ uint32_t(c)) == 0);          // CPU checksum folds to zero
        assert(R32(e.data() + 36) == cmdq_seq_seen++);             // element sequence numbers
        const uint32_t len = R32(e.data() + 48 + 8);
        assert(R32(e.data() + 48) == 0x03000000 && R32(e.data() + 52) == rm::RPC_SIGNATURE);
        assert((48 + len + 0xfff) / 0x1000 == count);
        rpc->assign(e.begin() + 48, e.begin() + 48 + len);
        A32w(msgq_ + 32, (r + count) % 63);
        return true;
    }
    // Read a logical RPC, following CONTINUATION_RECORDs up to `total` payload bytes.
    bool ReadRpc(uint32_t total_payload, uint32_t* fn, std::vector<uint8_t>* payload, Budget& wait) {
        std::vector<uint8_t> e;
        if (!ReadElement(&e, wait)) return false;
        *fn = R32(e.data() + 12);
        payload->assign(e.begin() + 32, e.end());
        while (payload->size() < total_payload) {
            if (!ReadElement(&e, wait)) return false;
            assert(R32(e.data() + 12) == rm::NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD);
            payload->insert(payload->end(), e.begin() + 32, e.end());
            ++continuations_seen;
        }
        return true;
    }
    // Write one element to msgq.
    bool WriteElement(uint32_t fn, const uint8_t* payload, uint32_t n, uint32_t result, Budget& wait,
                      bool corrupt = false) {
        const uint32_t len = 32 + n, count = (48 + len + 0xfff) / 0x1000;
        std::vector<uint8_t> e(count * 0x1000, 0);
        W32(e.data() + 36, msg_seq_++); W32(e.data() + 40, count);
        W32(e.data() + 48, 0x03000000); W32(e.data() + 52, rm::RPC_SIGNATURE); W32(e.data() + 56, len);
        W32(e.data() + 60, fn); W32(e.data() + 64, result);
        if (n) std::memcpy(e.data() + 80, payload, n);
        uint64_t c = 0, w;
        for (size_t i = 0; i < e.size(); i += 8) { std::memcpy(&w, e.data() + i, 8); c ^= w; }
        W32(e.data() + 32, uint32_t(c >> 32) ^ uint32_t(c) ^ (corrupt ? 1u : 0u));
        const uint32_t wp = A32(msgq_ + 16);
        while (62 - (wp + 63 - A32(cmdq_ + 32)) % 63 < count) if (!wait()) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t s = (wp + i) % 63;
            if (s < wp) straddled_msgq = true;
            std::memcpy(Slot(msgq_, s), e.data() + i * 0x1000, 0x1000);
        }
        A32w(msgq_ + 16, (wp + count) % 63);
        return true;
    }
    // Large message: first element (max payload) + continuations.
    bool WriteLarge(uint32_t fn, const std::vector<uint8_t>& p, Budget& wait) {
        uint32_t first = uint32_t(p.size()) > kMaxPayloadBytes ? kMaxPayloadBytes : uint32_t(p.size());
        if (!WriteElement(fn, p.data(), first, 0, wait)) return false;
        for (uint32_t off = first; off < p.size();) {
            const uint32_t n = uint32_t(p.size()) - off > kMaxPayloadBytes ? kMaxPayloadBytes : uint32_t(p.size()) - off;
            if (!WriteElement(rm::NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD, p.data() + off, n, 0, wait)) return false;
            off += n;
        }
        return true;
    }

    bool straddled_cmdq = false, straddled_msgq = false;
    uint32_t continuations_seen = 0;

private:
    uint8_t* cmdq_;
    uint8_t* msgq_;
    uint32_t cmdq_seq_seen = 0, msg_seq_ = 0;
};

std::vector<uint8_t> Pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) { seed = seed * 1664525 + 1013904223; v[i] = uint8_t(seed >> 24); }
    return v;
}

struct Fixture {
    std::vector<uint8_t> mem;
    QueueShm q;
    std::unique_ptr<RpcChannel> ch;
    std::unique_ptr<FakeGsp> gsp;
    Fixture() : mem(QueueShmBytes() + 0x1000) {
        uint8_t* base = reinterpret_cast<uint8_t*>((uintptr_t(mem.data()) + 0xfff) & ~uintptr_t(0xfff));
        assert(InitQueueShm(base, 0x10a000000ull, QueueShmBytes(), &q));
        ch = std::make_unique<RpcChannel>(q);
        gsp = std::make_unique<FakeGsp>(q);
    }
};

void TestLayout() {
    Fixture f;
    assert(QueueShmPteCount() == 129 && QueueShmBytes() == 0x81000);
    assert(f.q.ptes_size == 0x1000 && f.q.cmdq_off == 0x1000 && f.q.msgq_off == 0x41000);
    uint64_t pte;
    std::memcpy(&pte, f.q.base + 8 * 128, 8);
    assert(pte == 0x10a000000ull + 128 * 0x1000);
    rm::msgqTxHeader tx;
    std::memcpy(&tx, f.q.cmdq(), sizeof(tx));
    assert(tx.version == 0 && tx.size == 0x40000 && tx.msgSize == 0x1000 && tx.msgCount == 63 &&
           tx.writePtr == 0 && tx.flags == 1 && tx.rxHdrOff == 32 && tx.entryOff == 0x1000);
    uint8_t small[16];
    QueueShm bad;
    assert(!InitQueueShm(small, 0, sizeof(small), &bad));
    assert(!InitQueueShm(f.q.base, 0x10a000800ull, QueueShmBytes(), &bad));
}

// Out-of-order events before the reply, then the reply.
void TestEventsThenReply() {
    Fixture f;
    std::thread peer([&] {
        Budget w;
        uint32_t fn; std::vector<uint8_t> p;
        assert(f.gsp->ReadRpc(0, &fn, &p, w) && fn == rm::NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO && p.size() == 0x3a0);
        assert(f.gsp->ReadRpc(0, &fn, &p, w) && fn == rm::NV_VGPU_MSG_FUNCTION_SET_REGISTRY && p.size() == 0x75);
        const std::vector<uint8_t> seq = Pattern(0x7d0, 7), nocat = Pattern(0x4bc, 9);
        assert(f.gsp->WriteElement(rm::NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER, seq.data(), uint32_t(seq.size()), 0, w));
        for (int i = 0; i < 6; ++i)
            assert(f.gsp->WriteElement(rm::NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD, nocat.data(), uint32_t(nocat.size()), 0, w));
        assert(f.gsp->WriteElement(rm::NV_VGPU_MSG_EVENT_GSP_INIT_DONE, nullptr, 0, 0, w));
    });
    Budget w; Bell bell;
    const std::vector<uint8_t> si = Pattern(0x3a0, 1), reg = Pattern(0x75, 2);
    assert(f.ch->Send(rm::NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, si.data(), uint32_t(si.size()), w, bell, true) == QStatus::Ok);
    assert(f.ch->Send(rm::NV_VGPU_MSG_FUNCTION_SET_REGISTRY, reg.data(), uint32_t(reg.size()), w, bell, true) == QStatus::Ok);
    assert(bell.rings == 2 && f.ch->rpc_seq() == 0 && f.ch->cmdq_seq() == 2);
    std::vector<uint32_t> events;
    auto on_event = [&](const uint8_t* b, uint32_t len) {
        events.push_back(R32(b + 12));
        if (R32(b + 12) == rm::NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER)
            assert(len == 32 + 0x7d0 && std::memcmp(b + 32, Pattern(0x7d0, 7).data(), 0x7d0) == 0);
        return true;
    };
    std::vector<uint8_t> buf(0x20000);
    uint32_t len = 0;
    assert(f.ch->WaitFor(rm::NV_VGPU_MSG_EVENT_GSP_INIT_DONE, buf.data(), uint32_t(buf.size()), &len, 0, w, on_event) == QStatus::Ok);
    assert(len == 32 && events.size() == 7 && events[0] == rm::NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER);
    for (int i = 1; i < 7; ++i) assert(events[size_t(i)] == rm::NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD);
    assert(!f.ch->MsgPending());
    peer.join();
}

// Many RPCs of 1..16 slots, echoed back: both rings wrap repeatedly and
// elements straddle the last slot.
void TestWrapAround() {
    Fixture f;
    const int kRounds = 300;
    std::thread peer([&] {
        Budget w(20000);
        for (int i = 0; i < kRounds; ++i) {
            uint32_t fn; std::vector<uint8_t> p;
            assert(f.gsp->ReadRpc(0, &fn, &p, w));
            assert(fn == rm::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL);
            for (auto& c : p) c ^= 0x5a;
            assert(f.gsp->WriteElement(fn, p.data(), uint32_t(p.size()), 0, w));
        }
    });
    Budget w(20000); Bell bell;
    std::vector<uint8_t> buf(kMaxRpcBytes);
    for (int i = 0; i < kRounds; ++i) {
        const uint32_t n = uint32_t((i * 9973) % kMaxPayloadBytes);
        const std::vector<uint8_t> p = Pattern(n, uint32_t(i));
        assert(f.ch->Send(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, p.data(), n, w, bell) == QStatus::Ok);
        uint32_t len = 0;
        auto no_events = [](const uint8_t*, uint32_t) { return false; };
        assert(f.ch->WaitFor(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, buf.data(), uint32_t(buf.size()), &len, 0, w,
                             no_events) == QStatus::Ok);
        assert(len == 32 + n);
        for (uint32_t k = 0; k < n; ++k) assert(buf[32 + k] == (p[k] ^ 0x5a));
    }
    peer.join();
    assert(f.gsp->straddled_cmdq && f.gsp->straddled_msgq);
    assert(f.ch->rpc_seq() == uint32_t(kRounds));
}

// 200 KiB payload each way: first element + CONTINUATION_RECORDs.
void TestContinuation() {
    Fixture f;
    const std::vector<uint8_t> big = Pattern(200 * 1024, 42), reply = Pattern(150 * 1024 + 3, 43);
    std::thread peer([&] {
        Budget w;
        uint32_t fn; std::vector<uint8_t> p;
        assert(f.gsp->ReadRpc(uint32_t(big.size()), &fn, &p, w));
        assert(fn == rm::NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC && p == big);
        assert(f.gsp->continuations_seen == 3);               // 65456 * 3 + 9832
        // an unrelated event first, then the large reply
        assert(f.gsp->WriteElement(rm::NV_VGPU_MSG_EVENT_OS_ERROR_LOG, nullptr, 0, 0, w));
        assert(f.gsp->WriteLarge(fn, reply, w));
    });
    Budget w; Bell bell;
    assert(f.ch->Send(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, big.data(), uint32_t(big.size()), w, bell) == QStatus::Ok);
    assert(bell.rings == 4 && f.ch->rpc_seq() == 4);
    std::vector<uint8_t> buf(256 * 1024);
    uint32_t len = 0;
    int events = 0;
    auto on_event = [&](const uint8_t* b, uint32_t) { ++events; return R32(b + 12) == rm::NV_VGPU_MSG_EVENT_OS_ERROR_LOG; };
    assert(f.ch->WaitFor(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, buf.data(), uint32_t(buf.size()), &len,
                         uint32_t(32 + reply.size()), w, on_event) == QStatus::Ok);
    assert(events == 1 && len == 32 + reply.size() && R32(buf.data() + 8) == len);
    assert(std::memcmp(buf.data() + 32, reply.data(), reply.size()) == 0);
    peer.join();
}

void TestErrors() {
    {   // RPC error result
        Fixture f;
        Budget w;
        assert(f.gsp->WriteElement(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, nullptr, 0, 0x51, w));
        std::vector<uint8_t> buf(0x1000); uint32_t len = 0;
        auto ev = [](const uint8_t*, uint32_t) { return true; };
        assert(f.ch->WaitFor(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, buf.data(), 0x1000, &len, 0, w, ev) == QStatus::RpcError);
    }
    {   // checksum corruption is detected and the element is not consumed
        Fixture f;
        Budget w;
        assert(f.gsp->WriteElement(0x1001, nullptr, 0, 0, w, true));
        std::vector<uint8_t> buf(0x1000); uint32_t len = 0;
        assert(f.ch->Recv(buf.data(), 0x1000, &len, 0, w) == QStatus::BadChecksum);
        assert(f.ch->MsgPending());
    }
    {   // timeout with nothing queued
        Fixture f;
        Budget w(20);
        std::vector<uint8_t> buf(0x1000); uint32_t len = 0;
        assert(f.ch->Recv(buf.data(), 0x1000, &len, 0, w) == QStatus::Timeout);
    }
    {   // a non-continuation where a continuation is due
        Fixture f;
        Budget w;
        const std::vector<uint8_t> p = Pattern(kMaxPayloadBytes, 5);
        assert(f.gsp->WriteElement(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, p.data(), uint32_t(p.size()), 0, w));
        assert(f.gsp->WriteElement(rm::NV_VGPU_MSG_EVENT_POST_EVENT, nullptr, 0, 0, w));
        std::vector<uint8_t> buf(0x30000); uint32_t len = 0;
        assert(f.ch->Recv(buf.data(), uint32_t(buf.size()), &len, 0x20000, w) == QStatus::NotContinuation);
    }
    {   // receive buffer too small
        Fixture f;
        Budget w;
        const std::vector<uint8_t> p = Pattern(0x2000, 6);
        assert(f.gsp->WriteElement(0x1003, p.data(), uint32_t(p.size()), 0, w));
        std::vector<uint8_t> buf(0x1000); uint32_t len = 0;
        assert(f.ch->Recv(buf.data(), 0x1000, &len, 0, w) == QStatus::Overflow);
    }
    {   // cmdq full: the GSP never consumes, Send times out instead of overwriting
        Fixture f;
        Budget w(50); Bell bell;
        const std::vector<uint8_t> p = Pattern(kMaxPayloadBytes, 8);
        QStatus s = QStatus::Ok;
        int sent = 0;
        while ((s = f.ch->Send(rm::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, p.data(), uint32_t(p.size()), w, bell)) == QStatus::Ok) ++sent;
        assert(s == QStatus::Timeout && sent == 3);            // 3 x 16 slots fit in 62 free
    }
}

}  // namespace

int main() {
    TestLayout();
    TestEventsThenReply();
    TestWrapAround();
    TestContinuation();
    TestErrors();
    std::printf("test_nv_gsp_queue: OK\n");
    return 0;
}
