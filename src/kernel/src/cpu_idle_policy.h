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


// ---------------------------------------------------------------------------
// Menu-style governor (`cpu idle state auto`). Per CPU it keeps the last
// CpuIdleHistoryLen idle intervals and predicts the next one conservatively
// (the second-shortest of the window), so a CPU that is waking often under
// load stays shallow while a quiet CPU drops to C6. A latency hold (set by
// latency-critical kernel paths, e.g. the GPU submission worker while a job
// is in flight) forces C1E regardless of the prediction.
// ---------------------------------------------------------------------------
constexpr uint32_t CpuIdleHistoryLen = 8;

struct CpuIdleGovernorConfig {
    uint32_t c3TargetUs = 100;   // C3 exit ~33 us on Broadwell; worth it above ~100 us
    uint32_t c6TargetUs = 400;   // C6 exit ~133 us; target residency ~3x the exit latency
};

struct CpuIdleHistory {
    uint32_t us[CpuIdleHistoryLen] = {};
    uint32_t count = 0, pos = 0;
};

inline void CpuIdleHistoryPush(CpuIdleHistory& h, uint32_t us) {
    h.us[h.pos] = us;
    h.pos = (h.pos + 1) % CpuIdleHistoryLen;
    if (h.count < CpuIdleHistoryLen) ++h.count;
}

// Second-shortest interval of a full window (shortest when only 2-7 samples);
// 0 until two samples exist, which keeps a fresh CPU shallow.
inline uint32_t CpuIdlePredictUs(const CpuIdleHistory& h) {
    if (h.count < 2) return 0;
    uint32_t a = 0xffffffffu, b = 0xffffffffu;
    for (uint32_t i = 0; i < h.count; ++i) {
        const uint32_t v = h.us[i];
        if (v < a) { b = a; a = v; } else if (v < b) b = v;
    }
    return h.count < CpuIdleHistoryLen ? a : b;
}

inline CpuIdleState CpuIdleGovern(uint32_t predictedUs, bool latencyHold, const CpuIdleGovernorConfig& cfg = {}) {
    if (latencyHold) return CpuIdleState::C1E;
    if (predictedUs >= cfg.c6TargetUs) return CpuIdleState::C6;
    if (predictedUs >= cfg.c3TargetUs) return CpuIdleState::C3;
    return CpuIdleState::C1E;
}

inline uint32_t CpuIdleTargetUs(CpuIdleState s, const CpuIdleGovernorConfig& cfg = {}) {
    return s == CpuIdleState::C6 ? cfg.c6TargetUs : s == CpuIdleState::C3 ? cfg.c3TargetUs : 0u;
}

enum class CpuIdleOutcome : uint8_t { Hit, TooDeep, TooShallow };

// Was the state chosen for an interval right in hindsight? Too deep: the CPU
// woke before the state's target residency. Too shallow: it slept long enough
// for a deeper available state (ignoring latency holds, which are deliberate).
inline CpuIdleOutcome CpuIdleJudge(CpuIdleState chosen, uint32_t actualUs, bool held,
                                   CpuIdleState deepest, const CpuIdleGovernorConfig& cfg = {}) {
    if (actualUs < CpuIdleTargetUs(chosen, cfg)) return CpuIdleOutcome::TooDeep;
    if (!held) {
        const CpuIdleState ideal = CpuIdleGovern(actualUs, false, cfg);
        const CpuIdleState capped = int(ideal) > int(deepest) ? deepest : ideal;
        if (int(capped) > int(chosen)) return CpuIdleOutcome::TooShallow;
    }
    return CpuIdleOutcome::Hit;
}

} // namespace brook
