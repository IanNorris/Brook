#include "profiler_stack_walk.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

using namespace brook;

// Model real stack VA classes without mapping privileged addresses on the host.
// The production walker, bounds checks and code predicate are used unchanged.
static constexpr uint64_t text = 0xffffffff80000000ULL;
static constexpr uint64_t module = 0xffffffff90000000ULL;
static bool Code(uint64_t pc)
{
    return ProfilerReturnAddress(pc, text, text + 0x10000,
                                 module, module + 0x10000000);
}

struct Stack {
    uint64_t base;
    uint64_t words[16]{};
    unsigned reads = 0;
    ProfilerStackBounds bounds() const { return {base, base + sizeof(words)}; }
    void frame(unsigned index, uint64_t next, uint64_t pc) {
        words[index * 2] = next;
        words[index * 2 + 1] = pc;
    }
    uint8_t walk(uint64_t start, uint64_t* output, uint8_t capacity,
                 bool skip = false) {
        return walkWith(bounds(), start, output, capacity, skip);
    }
    uint8_t walkWith(ProfilerStackBounds trusted, uint64_t start,
                     uint64_t* output, uint8_t capacity, bool skip = false) {
        return ProfilerWalkFrames(start, trusted, output, capacity, skip,
            [&](uint64_t address, uint64_t& next, uint64_t& pc) {
                // Any out-of-range read is a failure, even if no PC is emitted.
                assert(bounds().ContainsFrame(address));
                ++reads;
                unsigned offset = static_cast<unsigned>((address - base) / 8);
                next = words[offset];
                pc = words[offset + 1];
            }, Code);
    }
};

static void addressClassesAndSavedReturn()
{
    const uint64_t stacks[] = {
        0xffffc08000001000ULL, // dynamically allocated kernel stack below text
        0xffffffff81000000ULL, // static idle/bootstrap/IST-like BSS storage
        0xffff800000004000ULL, // separate high-half mapped stack storage
    };
    for (uint64_t base : stacks) {
        Stack s{base};
        s.frame(0, base + 16, text + 0x20);
        s.frame(1, base + 32, text + 0x20); // legitimate recursive same-PC frame
        s.frame(2, 0, module + 0x40);
        uint64_t out[8]{};
        assert(s.walk(base, out, 8) == 3);
        assert(out[0] == text + 0x20 && out[1] == text + 0x20);
        assert(out[2] == module + 0x40);
        // Context-switch capture already records frame 0's return address.
        out[0] = text + 0x20;
        assert(s.walk(base, out + 1, 7, true) == 2);
        assert(out[0] == text + 0x20 && out[1] == text + 0x20);
        assert(out[2] == module + 0x40);
        // Incoming task bounds cannot justify reading the outgoing stack.
        unsigned before = s.reads;
        assert(s.walkWith({base + 4096, base + 8192}, base, out, 8) == 0);
        assert(s.reads == before);
    }
    assert(!Code(0xffffffff81000000ULL)); // BSS is storage, not kernel text
    assert(!Code(0xffffc08000001000ULL));
    assert(!Code(0x12345000));
    assert(!Code(text + 0x10000));
    assert(!Code(module + 0x10000000));
}

static void malformedChainsAndLimits()
{
    Stack s{0xffffc08000100000ULL};
    uint64_t out[8]{};
    const uint64_t base = s.base;
    for (unsigned i = 0; i < 8; ++i)
        s.frame(i, base + (i + 1) * 16, text + i * 8);
    assert(s.walk(base, out, 8) == 8);
    assert(s.reads == 8);
    assert(s.walk(base + 112, out, 8) == 1); // exact final complete frame
    unsigned before = s.reads;
    assert(s.walk(base + 120, out, 8) == 0); // incomplete frame
    assert(s.walk(base - 8, out, 8) == 0);
    assert(s.walk(base + 1, out, 8) == 0);
    assert(s.walk(base, out, 0) == 0);
    assert(s.walkWith({base + 128, base}, base, out, 8) == 0);
    assert(s.walkWith({base, base}, base, out, 8) == 0);
    assert(s.reads == before);
    assert(s.walk(base, out, 2) == 2);
    s.frame(0, base, text); // self cycle
    assert(s.walk(base, out, 8) == 1);
    s.frame(0, base - 16, text); // backwards
    assert(s.walk(base, out, 8) == 1);
    s.frame(0, base + 16, text);
    s.frame(1, base, text + 8); // two-node cycle
    assert(s.walk(base, out, 8) == 2);
    s.frame(1, base + 256, text + 8); // jump into unowned adjacent stack
    assert(s.walk(base, out, 8) == 2);
    s.frame(0, base + 16, base); // stack VA as return address
    assert(s.walk(base, out, 8) == 0);
    // Unsigned subtraction avoids address+16 wrap accepting a truncated frame.
    ProfilerStackBounds nearMax{UINT64_MAX - 31, UINT64_MAX};
    assert(nearMax.ContainsFrame(UINT64_MAX - 23));
    assert(!nearMax.ContainsFrame(UINT64_MAX - 7));
    assert((!ProfilerStackBounds{UINT64_MAX - 31, 16}.ContainsFrame(UINT64_MAX - 23)));
}

static void residentStorageReader()
{
    alignas(16) uint64_t frames[6]{};
    const uint64_t base = reinterpret_cast<uint64_t>(frames);
    frames[0] = base + 16;
    frames[1] = text + 8;
    frames[2] = base + 32;
    frames[3] = text + 16;
    frames[4] = 0;
    frames[5] = module + 8;
    uint64_t output[4]{};
    const auto count = ProfilerWalkFrames(base, {base, base + sizeof(frames)},
        output, 4, false,
        [](uint64_t address, uint64_t& next, uint64_t& pc) {
            const auto* frame = reinterpret_cast<const uint64_t*>(address);
            next = frame[0];
            pc = frame[1];
        }, Code);
    assert(count == 3 && output[0] == text + 8 && output[1] == text + 16);
    assert(output[2] == module + 8 && output[3] == 0);
}

int main()
{
    addressClassesAndSavedReturn();
    malformedChainsAndLimits();
    residentStorageReader();
    puts("profiler stack walker: PASS");
}
