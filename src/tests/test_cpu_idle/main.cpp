// Host tests for the idle C-state policy (cpu_idle_policy.h).
#include "cpu_idle_policy.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace brook;

static CpuIdleCaps Bdw() {
    CpuIdleCaps c{};
    c.mwait = true; c.leaf5 = true; c.extensions = true; c.interruptBreak = true; c.arat = true;
    // CPUID.05H:EDX = 0x11142120 (nibbles from bit 0: C0 0, C1 2, C2 1, C3 2, C4 4, C5 1, C6 1, C7 1)
    c.leaf5Edx = 0x11142120u;
    return c;
}

int main() {
    auto c = Bdw();
    assert(CpuIdleSubStates(c, 1) == 2 && CpuIdleSubStates(c, 2) == 1 && CpuIdleSubStates(c, 3) == 2 && CpuIdleSubStates(c, 4) == 4);
    assert(CpuIdleHint(c, CpuIdleState::C1) == 0x00);
    assert(CpuIdleHint(c, CpuIdleState::C1E) == 0x01);
    assert(CpuIdleHint(c, CpuIdleState::C3) == 0x10);
    assert(CpuIdleHint(c, CpuIdleState::C6) == 0x20);
    assert(CpuIdleHint(c, CpuIdleState::Hlt) == -1);

    // Default and explicit hlt never use MWAIT.
    auto h = CpuIdleSelect(c, CpuIdleState::Hlt);
    assert(h.state == CpuIdleState::Hlt && h.hint == -1);

    auto s6 = CpuIdleSelect(c, CpuIdleState::C6);
    assert(s6.state == CpuIdleState::C6 && s6.hint == 0x20 && s6.ecx == 1 && !strcmp(s6.clamp, "none"));

    // No interrupt-break extension: ECX 0 (IF=1 still wakes on interrupts).
    auto nb = c; nb.interruptBreak = false;
    assert(CpuIdleSelect(nb, CpuIdleState::C6).ecx == 0);

    // No ARAT: the LAPIC timer stops in C3+, so clamp to C1E.
    auto na = c; na.arat = false;
    auto ca = CpuIdleSelect(na, CpuIdleState::C6);
    assert(ca.state == CpuIdleState::C1E && ca.hint == 0x01 && !strcmp(ca.clamp, "no-arat"));
    assert(CpuIdleSelect(na, CpuIdleState::C3).state == CpuIdleState::C1E);
    assert(CpuIdleSelect(na, CpuIdleState::C1).state == CpuIdleState::C1);

    // No MWAIT (VMs, KVM): always HLT.
    auto vm = c; vm.mwait = false;
    auto cv = CpuIdleSelect(vm, CpuIdleState::C6);
    assert(cv.state == CpuIdleState::Hlt && cv.hint == -1 && !strcmp(cv.clamp, "no-mwait"));
    auto nl = c; nl.leaf5 = false;
    assert(CpuIdleSelect(nl, CpuIdleState::C6).hint == -1);

    // Missing C6 sub-state steps down to C3, then C1E.
    auto m = c; m.leaf5Edx = 0x00000120u;   // C1 2, C2 1, no C3 index
    auto cm = CpuIdleSelect(m, CpuIdleState::C6);
    assert(cm.state == CpuIdleState::C3 && cm.hint == 0x10 && !strcmp(cm.clamp, "substate-missing"));
    m.leaf5Edx = 0x00000020u;               // only C1 sub-states
    assert(CpuIdleSelect(m, CpuIdleState::C6).state == CpuIdleState::C1E);
    m.leaf5Edx = 0;
    auto none = CpuIdleSelect(m, CpuIdleState::C6);
    assert(none.state == CpuIdleState::Hlt && none.hint == -1 && !strcmp(none.clamp, "no-substates"));

    // EPB: only 0 (performance) or 15 are treated as unset -> 6.
    assert(CpuEpbBootTarget(0) == 6 && CpuEpbBootTarget(15) == 6);
    assert(CpuEpbBootTarget(6) == -1 && CpuEpbBootTarget(4) == -1 && CpuEpbBootTarget(0x70) == 6);


    // ---- Governor ----
    CpuIdleGovernorConfig cfg{};
    CpuIdleHistory hh{};
    assert(CpuIdlePredictUs(hh) == 0);                       // fresh CPU: shallow
    assert(CpuIdleGovern(CpuIdlePredictUs(hh), false) == CpuIdleState::C1E);
    CpuIdleHistoryPush(hh, 900);
    assert(CpuIdlePredictUs(hh) == 0);                       // one sample is not enough
    CpuIdleHistoryPush(hh, 950);
    assert(CpuIdlePredictUs(hh) == 900);                     // partial window: shortest
    // A quiet CPU (ticks every ~1 ms) goes to C6.
    for (int i = 0; i < 8; ++i) CpuIdleHistoryPush(hh, 980);
    assert(CpuIdlePredictUs(hh) == 980 && CpuIdleGovern(980, false) == CpuIdleState::C6);
    // One short wake in the window does not change the prediction (second-shortest)...
    CpuIdleHistoryPush(hh, 30);
    assert(CpuIdlePredictUs(hh) == 980);
    // ...two do: a busy CPU stays shallow.
    CpuIdleHistoryPush(hh, 40);
    assert(CpuIdlePredictUs(hh) == 40 && CpuIdleGovern(40, false) == CpuIdleState::C1E);
    // Medium intervals pick C3.
    CpuIdleHistory mh{};
    for (int i = 0; i < 8; ++i) CpuIdleHistoryPush(mh, 200);
    assert(CpuIdleGovern(CpuIdlePredictUs(mh), false) == CpuIdleState::C3);
    // Thresholds are inclusive at the target residency.
    assert(CpuIdleGovern(cfg.c6TargetUs, false) == CpuIdleState::C6);
    assert(CpuIdleGovern(cfg.c6TargetUs - 1, false) == CpuIdleState::C3);
    assert(CpuIdleGovern(cfg.c3TargetUs - 1, false) == CpuIdleState::C1E);
    // A latency hold always wins.
    assert(CpuIdleGovern(5000, true) == CpuIdleState::C1E);
    // The window really is a ring of the last 8 samples.
    CpuIdleHistory rh{};
    for (int i = 0; i < 8; ++i) CpuIdleHistoryPush(rh, 10);
    for (int i = 0; i < 7; ++i) CpuIdleHistoryPush(rh, 700);
    assert(CpuIdlePredictUs(rh) == 700);   // [10, 700 x7]: second-shortest
    CpuIdleHistoryPush(rh, 700);
    assert(CpuIdlePredictUs(rh) == 700);

    // ---- Judging ----
    assert(CpuIdleJudge(CpuIdleState::C6, 100, false, CpuIdleState::C6) == CpuIdleOutcome::TooDeep);
    assert(CpuIdleJudge(CpuIdleState::C6, 900, false, CpuIdleState::C6) == CpuIdleOutcome::Hit);
    assert(CpuIdleJudge(CpuIdleState::C1E, 900, false, CpuIdleState::C6) == CpuIdleOutcome::TooShallow);
    assert(CpuIdleJudge(CpuIdleState::C1E, 900, true, CpuIdleState::C6) == CpuIdleOutcome::Hit);   // hold is deliberate
    assert(CpuIdleJudge(CpuIdleState::C1E, 50, false, CpuIdleState::C6) == CpuIdleOutcome::Hit);
    assert(CpuIdleJudge(CpuIdleState::C3, 900, false, CpuIdleState::C3) == CpuIdleOutcome::Hit);    // capped by deepest
    assert(CpuIdleJudge(CpuIdleState::C3, 50, false, CpuIdleState::C6) == CpuIdleOutcome::TooDeep);

    // Simulated load: alternating short GPU-poll wakes and long ticks keeps
    // the conservative predictor shallow; pure idle converges to C6.
    CpuIdleHistory load{};
    int deep = 0;
    for (int i = 0; i < 200; ++i) {
        const CpuIdleState s = CpuIdleGovern(CpuIdlePredictUs(load), false);
        if (s == CpuIdleState::C6) ++deep;
        CpuIdleHistoryPush(load, (i % 3) ? 80 : 1000);
    }
    assert(deep == 0);
    CpuIdleHistory quiet{};
    int quietDeep = 0;
    for (int i = 0; i < 200; ++i) {
        if (CpuIdleGovern(CpuIdlePredictUs(quiet), false) == CpuIdleState::C6) ++quietDeep;
        CpuIdleHistoryPush(quiet, 990);
    }
    assert(quietDeep >= 198);

    for (uint32_t i = 0; i < uint32_t(CpuIdleState::Count); ++i) assert(CpuIdleStateName(CpuIdleState(i))[0] != '?');
    puts("test_cpu_idle: ok");
    return 0;
}
