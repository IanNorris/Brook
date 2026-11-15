// Last-log records: round trip, corruption and torn-write rejection, slot
// alternation, cross-boot sequence continuation and previous-record choice.
#include "../../kernel/src/lastlog_format.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK failed %s:%d %s\n",__FILE__,__LINE__,#x); std::abort(); } } while(0)
using namespace brook;

namespace {
std::vector<uint8_t> Make(uint64_t seq, const char* boot, const char* text)
{
    std::vector<uint8_t> f(LastLogFileBytes);
    LastLogEncode(f.data(), seq, boot, reinterpret_cast<const uint8_t*>(text), static_cast<uint32_t>(std::strlen(text)));
    return f;
}
const char* kBootA = "0123456789abcdef0123456789abcdef";
const char* kBootB = "fedcba9876543210fedcba9876543210";
}

int main()
{
    // Round trip.
    auto a = Make(7, kBootA, "line one\nline two\n");
    LastLogRecord r = LastLogDecode(a.data(), LastLogFileBytes);
    CHECK(r.valid && r.sequence == 7 && r.payloadBytes == 18);
    CHECK(std::strcmp(r.bootId, kBootA) == 0);
    CHECK(std::memcmp(a.data() + LastLogHeaderBytes, "line one\n", 9) == 0);
    for (uint32_t i = LastLogHeaderBytes + 18; i < LastLogFileBytes; ++i) CHECK(a[i] == 0);

    // Blank media, wrong magic, flipped payload bit, flipped header bit, short buffer.
    std::vector<uint8_t> blank(LastLogFileBytes, 0);
    CHECK(!LastLogDecode(blank.data(), LastLogFileBytes).valid);
    auto bad = a; bad[LastLogHeaderBytes + 3] ^= 1;
    CHECK(!LastLogDecode(bad.data(), LastLogFileBytes).valid);
    bad = a; bad[17] ^= 0x10;
    CHECK(!LastLogDecode(bad.data(), LastLogFileBytes).valid);
    bad = a; bad[0] = 'X';
    CHECK(!LastLogDecode(bad.data(), LastLogFileBytes).valid);
    CHECK(!LastLogDecode(a.data(), 32).valid);
    CHECK(!LastLogDecode(nullptr, LastLogFileBytes).valid);

    // Torn write: new header over old payload, and old header over new payload.
    auto older = Make(8, kBootA, "older record payload");
    auto newer = Make(10, kBootA, "newer record payload!!");
    auto torn = older; std::memcpy(torn.data(), newer.data(), LastLogHeaderBytes);
    CHECK(!LastLogDecode(torn.data(), LastLogFileBytes).valid);
    torn = newer; std::memcpy(torn.data(), older.data(), LastLogHeaderBytes);
    CHECK(!LastLogDecode(torn.data(), LastLogFileBytes).valid);

    // Oversized payload is clamped and still valid.
    std::vector<uint8_t> big(LastLogFileBytes * 2, 'x');
    std::vector<uint8_t> f(LastLogFileBytes);
    LastLogEncode(f.data(), 3, kBootB, big.data(), static_cast<uint32_t>(big.size()));
    r = LastLogDecode(f.data(), LastLogFileBytes);
    CHECK(r.valid && r.payloadBytes == LastLogPayloadMax);

    // Slot choice and continuation across boots. Boot A wrote 1..5 (slot 1
    // holds 5, slot 0 holds 4). Boot B starts at 6 and reads A's 5 as its
    // previous record, then writes slot 0, then slot 1.
    LastLogRecord s0 = LastLogDecode(Make(4, kBootA, "a4").data(), LastLogFileBytes);
    LastLogRecord s1 = LastLogDecode(Make(5, kBootA, "a5").data(), LastLogFileBytes);
    CHECK(LastLogNewest(s0, s1) == 1);
    CHECK(LastLogNextSequence(s0, s1) == 6);
    CHECK(LastLogSlotFor(5) == 1 && LastLogSlotFor(6) == 0 && LastLogSlotFor(7) == 1);
    // A torn first write of boot B (slot 0) still leaves A's newest intact.
    LastLogRecord tornSlot{};
    CHECK(LastLogNewest(tornSlot, s1) == 1);
    CHECK(LastLogNextSequence(tornSlot, s1) == 6);
    // Blank media starts at 1; equal sequences are ambiguous.
    LastLogRecord none{};
    CHECK(LastLogNewest(none, none) == -1 && LastLogNextSequence(none, none) == 1);
    LastLogRecord dup = s1;
    CHECK(LastLogNewest(dup, s1) == -1);
    // Sequence saturation never wraps to zero.
    LastLogRecord top = s1; top.sequence = ~uint64_t(0);
    CHECK(LastLogNextSequence(top, none) == ~uint64_t(0));

    std::puts("test_lastlog_format: ok");
    return 0;
}
