#pragma once
#include <stdint.h>

namespace brook {

// Exclusive upper bound. Storage must stay mapped and owned by the caller for
// the entire walk. Selecting a range is not permission to follow another stack.
struct ProfilerStackBounds {
    uint64_t base;
    uint64_t end;
    bool ContainsFrame(uint64_t address) const {
        return base < end && address >= base && address < end &&
               end - address >= 2 * sizeof(uint64_t) && (address & 7) == 0;
    }
};

inline bool ProfilerReturnAddress(uint64_t pc, uint64_t textBegin,
                                  uint64_t textEnd, uint64_t moduleBegin,
                                  uint64_t moduleEnd)
{
    // The module arena is conservative: it does not prove executable text.
    return (pc >= textBegin && pc < textEnd) ||
           (pc >= moduleBegin && pc < moduleEnd);
}

// Reader is called only for a complete, aligned frame inside ONE trusted range.
// skipFirstReturn is for a capture whose leaf is __builtin_return_address(0):
// skip that frame's return slot once, not equal PC values in recursive callers.
template<class Reader, class CodePredicate>
uint8_t ProfilerWalkFrames(uint64_t rbp, ProfilerStackBounds bounds,
                           uint64_t* output, uint8_t capacity,
                           bool skipFirstReturn, Reader read, CodePredicate code)
{
    uint8_t depth = 0;
    while (depth < capacity && bounds.ContainsFrame(rbp)) {
        uint64_t next, pc;
        read(rbp, next, pc);
        if (skipFirstReturn) {
            skipFirstReturn = false;
        } else {
            if (!code(pc)) break;
            output[depth++] = pc;
        }
        if (next <= rbp) break;
        rbp = next;
    }
    return depth;
}

} // namespace brook
