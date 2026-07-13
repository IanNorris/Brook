// panic_unwind.cpp — robust, fault-tolerant kernel stack unwinder.
// See panic_unwind.h for rationale.

#include "panic_unwind.h"
#include "panic_probe.h"

extern "C" char __etext[];

namespace brook {

namespace {

constexpr uint64_t KTEXT_LO = 0xFFFFFFFF80000000ULL;
constexpr uint64_t KCANON_LO = 0xFFFF800000000000ULL;  // kernel-half canonical base

// How many bytes of stack to scan for call-preceded return addresses when the
// RBP chain breaks. 4 KB covers deep-enough frames without burning time.
constexpr uint64_t SCAN_WINDOW_BYTES = 4096;

inline uint64_t KtextHi()
{
    return reinterpret_cast<uint64_t>(__etext);
}

inline bool InKernelText(uint64_t addr)
{
    return addr >= KTEXT_LO && addr < KtextHi();
}

// Is `ret` immediately preceded by a call instruction? A genuine return address
// always follows a call, so this is the single check that separates a real
// stack-scan hit from a pushed .text pointer that happens to look like code.
//   * E8 rel32  — direct near call (5 bytes): byte at ret-5 == 0xE8.
//   * FF /2     — indirect near call (2..7 bytes): an 0xFF opcode with ModRM.reg
//                 == 2 close before ret. We check the common 2/3-byte encodings.
bool PrecededByCall(uint64_t ret)
{
    // Direct call: E8 rel32.
    uint64_t v;
    if (PanicSafeReadU64(ret - 5, &v))
    {
        if ((v & 0xFF) == 0xE8)
            return true;
    }
    // Indirect call FF /2. Read the up-to-7 bytes before `ret` and look for an
    // 0xFF whose following ModRM byte selects reg field 2 (call r/m64).
    uint8_t buf[8];
    if (PanicSafeCopy(buf, ret - 7, 7) == 7)
    {
        // buf[0..6] correspond to ret-7 .. ret-1. An FF-form call is 2..7 bytes
        // and ends exactly at `ret`, so the 0xFF can sit at offsets 6..0 with a
        // matching ModRM immediately after it, still within the window.
        for (int i = 6; i >= 0; --i)
        {
            if (buf[i] != 0xFF) continue;
            if (i + 1 > 6) continue;                 // need a ModRM byte after FF
            uint8_t modrm = buf[i + 1];
            if (((modrm >> 3) & 0x7) == 0x2)         // reg field == 2 -> CALL r/m
                return true;
        }
    }
    return false;
}

}  // namespace

uint32_t PanicUnwindStack(PanicUnwindFrame* out, uint32_t maxFrames,
                          uint64_t rip, uint64_t rbp, uint64_t rsp,
                          uint64_t stackLo, uint64_t stackHi)
{
    if (!out || maxFrames == 0) return 0;

    // Default to a broad kernel-half range when precise bounds are unknown. Safe
    // because every read goes through PanicSafe* and return addresses are
    // .text/call-validated regardless.
    if (stackLo == 0 || stackHi == 0 || stackHi <= stackLo)
    {
        stackLo = KCANON_LO;
        stackHi = 0xFFFFFFFFFFFFF000ULL;
    }

    uint32_t n = 0;

    // Frame 0: the interrupted RIP (EXACT), if it looks like kernel code.
    if (InKernelText(rip) && n < maxFrames)
        out[n++] = PanicUnwindFrame{ rip, PANIC_FRAME_EXACT };

    // Phase 1: validated RBP chain.
    uint64_t cur = rbp;
    uint64_t prev = 0;
    while (n < maxFrames)
    {
        if (cur < stackLo || cur >= stackHi - 16 || (cur & 0x7) != 0)
            break;
        if (prev != 0 && cur <= prev)     // must climb monotonically (stack grows down)
            break;

        uint64_t savedRbp = 0, retAddr = 0;
        if (!PanicSafeReadU64(cur, &savedRbp))       break;   // frame[0] = caller RBP
        if (!PanicSafeReadU64(cur + 8, &retAddr))    break;   // frame[1] = return addr
        if (!InKernelText(retAddr))                  break;

        out[n++] = PanicUnwindFrame{ retAddr, PANIC_FRAME_RBP };
        prev = cur;
        cur = savedRbp;
    }

    // Phase 2: if the RBP chain gave us little, scan the stack for call-preceded
    // .text pointers. This recovers a backtrace when frame pointers are omitted
    // or RBP is corrupt. Every hit is flagged heuristic.
    if (n < maxFrames)
    {
        uint64_t scanEnd = rsp + SCAN_WINDOW_BYTES;
        if (scanEnd > stackHi) scanEnd = stackHi;
        // Start above the words the RBP walk already trusted; simplest correct
        // choice is to scan the whole window and de-dupe against emitted frames.
        for (uint64_t p = rsp & ~0x7ULL; p + 8 <= scanEnd && n < maxFrames; p += 8)
        {
            uint64_t v = 0;
            if (!PanicSafeReadU64(p, &v)) continue;
            if (!InKernelText(v)) continue;
            if (!PrecededByCall(v)) continue;

            bool dup = false;
            for (uint32_t i = 0; i < n; ++i)
                if (out[i].rip == v) { dup = true; break; }
            if (dup) continue;

            out[n++] = PanicUnwindFrame{ v, PANIC_FRAME_SCAN };
        }
    }

    return n;
}

}  // namespace brook
