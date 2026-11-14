#pragma once
// Idle C-state policy (pure logic, host-tested). Decodes CPUID leaf 5/6 and
// selects the MWAIT hint IdleLoop uses. The default is HLT (C1), the pre-
// existing behaviour; deeper states are opt-in at runtime (`cpu idle`).
#include <stdint.h>

namespace brook {

enum class CpuIdleState : uint8_t { Hlt = 0, C1, C1E, C3, C6, Count };

inline const char* CpuIdleStateName(CpuIdleState s) {
    switch (s) {
        case CpuIdleState::Hlt: return "hlt";
        case CpuIdleState::C1: return "c1";
        case CpuIdleState::C1E: return "c1e";
        case CpuIdleState::C3: return "c3";
        case CpuIdleState::C6: return "c6";
        default: return "?";
    }
}

struct CpuIdleCaps {
    bool mwait = false;          // CPUID.01H:ECX[3] MONITOR/MWAIT, bare metal Intel
    bool leaf5 = false;          // CPUID.05H available
    bool extensions = false;     // CPUID.05H:ECX[0] MWAIT extensions enumerated
    bool interruptBreak = false; // CPUID.05H:ECX[1] interrupts break MWAIT even with IF=0
    bool arat = false;           // CPUID.06H:EAX[2] APIC timer always running (deep C-states)
    uint32_t leaf5Edx = 0;       // sub-state counts per MWAIT C-state, 4 bits each
};

// Number of MWAIT sub-states CPUID reports for MWAIT C-state index n (0..7);
// index 1 is C1, 2 is C2 (ACPI C3, hint 0x1x), 3 is C3 (C6, hint 0x2x).
inline uint32_t CpuIdleSubStates(const CpuIdleCaps& c, uint32_t index) {
    return index < 8 ? (c.leaf5Edx >> (index * 4)) & 0xfu : 0u;
}

// The MWAIT hint for a state (Intel bdw_cstates: C1 0x00, C1E 0x01, C3 0x10,
// C6 0x20), or -1 when CPUID does not advertise the needed sub-state.
inline int32_t CpuIdleHint(const CpuIdleCaps& c, CpuIdleState s) {
    if (!c.mwait || !c.leaf5) return -1;
    switch (s) {
        case CpuIdleState::C1:  return CpuIdleSubStates(c, 1) >= 1 ? 0x00 : -1;
        case CpuIdleState::C1E: return CpuIdleSubStates(c, 1) >= 2 ? 0x01 : -1;
        case CpuIdleState::C3:  return CpuIdleSubStates(c, 2) >= 1 ? 0x10 : -1;
        case CpuIdleState::C6:  return CpuIdleSubStates(c, 3) >= 1 ? 0x20 : -1;
        default: return -1;
    }
}

// States at or below C1E keep the LAPIC timer running on every part; C3 and
// deeper stop it unless ARAT is set, which would lose the scheduler tick.
inline bool CpuIdleNeedsArat(CpuIdleState s) { return s == CpuIdleState::C3 || s == CpuIdleState::C6; }

struct CpuIdleChoice {
    CpuIdleState state = CpuIdleState::Hlt;
    int32_t hint = -1;          // -1 = HLT
    uint32_t ecx = 0;           // MWAIT ECX (bit 0: break on interrupt when supported)
    const char* clamp = "none"; // why the request was lowered
};

// Choose the deepest available state at or below the request. Falls back to
// HLT whenever MWAIT is unusable; never selects a timer-stopping state without
// ARAT.
inline CpuIdleChoice CpuIdleSelect(const CpuIdleCaps& c, CpuIdleState requested) {
    CpuIdleChoice out{};
    if (requested == CpuIdleState::Hlt) return out;
    if (!c.mwait || !c.leaf5) { out.clamp = "no-mwait"; return out; }
    int s = int(requested);
    const bool aratClamp = CpuIdleNeedsArat(requested) && !c.arat;
    if (aratClamp) s = int(CpuIdleState::C1E);
    for (; s >= int(CpuIdleState::C1); --s) {
        const int32_t hint = CpuIdleHint(c, CpuIdleState(s));
        if (hint >= 0) {
            out.state = CpuIdleState(s);
            out.hint = hint;
            out.ecx = c.interruptBreak ? 1u : 0u;
            if (aratClamp) out.clamp = "no-arat";
            else if (s != int(requested)) out.clamp = "substate-missing";
            return out;
        }
    }
    out.clamp = "no-substates";
    return out;
}

// Energy-performance bias policy: Linux sets 6 (normal) when the firmware left
// 0 (performance); 15 is also treated as unset. Returns the value to write, or
// -1 to leave the register alone.
inline int32_t CpuEpbBootTarget(uint64_t current) {
    const uint32_t v = uint32_t(current & 0xfu);
    return (v == 0 || v == 15) ? 6 : -1;
}

} // namespace brook
