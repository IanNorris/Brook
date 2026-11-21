// Host tests for the idle C-state policy (cpu_idle_policy.h).
#include "cpu_idle_policy.h"
#include "cpu_epb_trial.h"
#include "cpu_power_budget.h"
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

static void TestRatioTrial()
{
    using Phase=CpuEpbTrial::Phase;
    using Outcome=CpuEpbTrial::Outcome;
    CpuEpbTrial trial;
    // The EPB default bound still rejects ratio-sized values.
    assert(!trial.Start(0,2,24,5000));
    assert(!trial.Start(0,2,256,5000,255));
    assert(!trial.Start(0,2,24,5000,256));
    unsigned ratio[]={30,30};
    auto tick=[&](unsigned cpu,uint64_t now){
        trial.Tick(cpu,now,true,[&](){return ratio[cpu];},[&](unsigned value){ratio[cpu]=value;return ratio[cpu];});
    };
    assert(trial.Start(0,2,24,2000,255));
    tick(0,1);tick(1,1);              // snapshot both
    assert(trial.phase==Phase::Apply);
    tick(0,2);tick(1,2);              // apply both
    assert(trial.phase==Phase::Hold && ratio[0]==24 && ratio[1]==24);
    assert(trial.original[0]==30 && trial.original[1]==30);
    tick(0,2003);                      // duration elapsed: restore
    assert(trial.phase==Phase::Restore && trial.outcome==Outcome::Complete);
    tick(0,2004);tick(1,2004);
    assert(trial.phase==Phase::Off && ratio[0]==30 && ratio[1]==30 && !trial.restoreFailed);
}

static void TestEpbTrial()
{
    using Phase=CpuEpbTrial::Phase;
    using Outcome=CpuEpbTrial::Outcome;
    CpuEpbTrial trial;
    unsigned bias[]={6,8},writes=0;
    bool failWrites=false;
    auto tick=[&](unsigned cpu,uint64_t now,bool eligible=true) {
        trial.Tick(cpu,now,eligible,[&](){return bias[cpu];},[&](unsigned value){
            ++writes;
            if(!failWrites)bias[cpu]=value;
            return bias[cpu];
        });
    };
    assert(!trial.Start(0,2,16,1000) && !trial.Start(0,9,10,1000));
    assert(trial.Start(0,2,10,1000) && !trial.Start(0,2,10,1000));
    tick(0,250);tick(0,500);
    assert(trial.phase==Phase::Snapshot && !writes);
    tick(1,500);
    assert(trial.phase==Phase::Apply && !writes && trial.original[0]==6 && trial.original[1]==8);
    tick(0,750);tick(1,1000);
    assert(trial.phase==Phase::Hold && writes==2 && trial.deadline==2000);
    tick(0,1999);tick(1,2000);tick(0,2250);
    assert(trial.phase==Phase::Off && trial.outcome==Outcome::Complete && bias[0]==6 && bias[1]==8 &&
           trial.restored==3 && !trial.restoreFailed);

    assert(trial.Start(3000,2,10,1000));
    tick(0,3250);tick(0,6000);
    assert(trial.phase==Phase::Off && trial.outcome==Outcome::Timeout && writes==4);

    assert(trial.Start(7000,2,10,1000));
    tick(0,7250);tick(1,7250);tick(0,7500);tick(1,7500,false);tick(0,7750,false);
    assert(trial.phase==Phase::Off && trial.outcome==Outcome::Ineligible && bias[0]==6 && bias[1]==8);

    assert(trial.Start(8000,2,10,1000));
    tick(0,8250);tick(1,8250);tick(0,8500);tick(1,8500);
    bias[1]=12;
    tick(1,8750);tick(1,9000);tick(0,9000);
    assert(trial.phase==Phase::Off && trial.outcome==Outcome::Superseded && bias[1]==12 && bias[0]==6 &&
           trial.restoreFailed==2);

    assert(trial.Start(10000,1,10,1000));tick(0,10250);tick(0,10500);
    failWrites=true;
    trial.Cancel();
    tick(0,10750);tick(0,11000);tick(0,11250);
    assert(trial.phase==Phase::Off && trial.outcome==Outcome::Cancelled && trial.restoreFailed==1);
}

static CpuBudgetSample Power(uint32_t mw, int32_t c = 70)
{
    CpuBudgetSample s{};
    s.powerValid = s.tempValid = true;
    s.coresMw = mw;
    s.pkgC = c;
    return s;
}

// Pure control law: Dell numbers (min 8, max non-turbo 24, firmware 30).
static void TestBudgetLaw()
{
    using R = CpuBudgetReason;
    const CpuBudgetLimits lim{8, 23, 30};
    CpuBudgetConfig cfg{3500, 0};
    CpuBudgetFilter f{};
    unsigned cap = 0;
    // Under budget while released: hold (nothing to raise).
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(2000)) == R::Band && cap == 0);
    // Invalid power sample: no decision.
    CpuBudgetSample bad = Power(9000); bad.powerValid = false;
    assert(CpuBudgetDecide(cfg, lim, f, cap, bad) == R::NoSample && cap == 0);
    // Over budget: needs two filtered samples, then released -> capTop (23).
    f = {};
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(5000)) == R::Wait || cap == 0);
    assert(cap == 0);
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(5000)) == R::OverPower && cap == 23);
    // The next sample spans the step and is skipped.
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(5000)) == R::Settling && cap == 23);
    // Still over: one bin at a time, rate-limited to every 3 samples.
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(4000)) == R::Wait && cap == 23);
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(4000)) == R::OverPower && cap == 22);
    // In the hysteresis band (3.06..3.5 W): hold indefinitely.
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(3300)) == R::Settling);
    for (int i = 0; i < 40; ++i) assert(CpuBudgetDecide(cfg, lim, f, cap, Power(3300)) != R::OverPower);
    assert(cap == 22 && f.mw == 3300);
    // Comfortably under: step up only after UpSamples filtered samples.
    unsigned ups = 0;
    for (int i = 0; i < 9; ++i) if (CpuBudgetDecide(cfg, lim, f, cap, Power(2500)) == R::StepUp) ++ups;
    assert(ups == 1 && cap == 23);
    // Release needs the turbo prediction (x(30/23)^2 ~ 1.7) to fit: 2.5 W -> 4.25 W does not.
    for (int i = 0; i < 30; ++i) CpuBudgetDecide(cfg, lim, f, cap, Power(2500));
    assert(cap == 23 && f.mw == 2500);
    // 1.8 W predicts ~3.06 W: release back to the firmware request.
    R last = R::None;
    for (int i = 0; i < 30 && cap; ++i) last = CpuBudgetDecide(cfg, lim, f, cap, Power(1800));
    assert(cap == 0 && last == R::Release);
    // Clamp at the floor.
    cap = 8; f = {};
    CpuBudgetDecide(cfg, lim, f, cap, Power(9000));
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(9000)) == R::AtFloor && cap == 8);
    // Out-of-range cap is clamped into minRatio..capTop.
    cap = 40; f = {};
    CpuBudgetDecide(cfg, lim, f, cap, Power(3300));
    assert(cap == 23);
    cap = 3; f = {};
    CpuBudgetDecide(cfg, lim, f, cap, Power(3300));
    assert(cap == 8);

    // Temperature target only: filtered temp over target after 8 samples.
    cfg = {0, 80};
    cap = 0; f = {};
    for (int i = 0; i < 7; ++i) assert(CpuBudgetDecide(cfg, lim, f, cap, Power(9000, 81)) == R::Band);
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(9000, 81)) == R::OverTemp && cap == 23);
    // Spike >= target + 5 steps immediately (after the settle sample).
    CpuBudgetDecide(cfg, lim, f, cap, Power(9000, 86));
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(9000, 86)) == R::HotEmergency && cap == 22);
    // Must be TempBandC under target to raise; power ignored without a power target.
    CpuBudgetDecide(cfg, lim, f, cap, Power(9000, 77));
    for (int i = 0; i < 12; ++i) assert(CpuBudgetDecide(cfg, lim, f, cap, Power(9000, 77)) != R::StepUp);
    assert(cap == 22);
    for (int i = 0; i < 20 && cap == 22; ++i) CpuBudgetDecide(cfg, lim, f, cap, Power(9000, 70));
    assert(cap == 23);
    // Both targets: either over steps down; both under needed to raise.
    cfg = {3500, 80};
    cap = 0; f = {};
    CpuBudgetDecide(cfg, lim, f, cap, Power(3000, 60));
    assert(CpuBudgetDecide(cfg, lim, f, cap, Power(3000, 60)) == R::Band && cap == 0);
    CpuBudgetDecide(cfg, lim, f, cap, Power(3000, 90)); // emergency on the instant reading
    assert(cap == 23);
    CpuBudgetSample noTemp = Power(3000, 60); noTemp.tempValid = false;
    assert(CpuBudgetDecide(cfg, lim, f, cap, noTemp) == R::NoSample);
    // Band never collapses below MinBandMw.
    assert(CpuBudgetLaw::BandMw(1000) == 200 && CpuBudgetLaw::BandMw(4000) == 500);
}

struct FakeCtl {
    unsigned ratio[4] = {30, 30, 30, 30};
    unsigned writes = 0;
    bool stuck = false;
};

static void TestBudgetApply()
{
    using Phase = CpuPowerBudget::Phase;
    using Outcome = CpuPowerBudget::Outcome;
    CpuPowerBudget b;
    FakeCtl hw;
    auto tick = [&](unsigned cpu, uint64_t now, bool eligible = true) {
        b.Tick(cpu, now, eligible, [&]() { return hw.ratio[cpu]; }, [&](unsigned v) {
            ++hw.writes;
            if (!hw.stuck) hw.ratio[cpu] = v;
            return hw.ratio[cpu];
        });
    };
    auto all = [&](uint64_t now, bool eligible = true) { for (unsigned c = 0; c < 4; ++c) tick(c, now, eligible); };
    auto over = [&](uint64_t now, int n) { for (int i = 0; i < n; ++i) { b.Control(now, Power(5000)); all(now); } };

    // Validation.
    assert(!b.Start(0, 4, {0, 0}, 8, 24));
    assert(!b.Start(0, 4, {500, 0}, 8, 24));
    assert(!b.Start(0, 4, {3500, 120}, 8, 24));
    assert(!b.Start(0, 9, {3500, 0}, 8, 24));
    assert(!b.Start(0, 4, {3500, 0}, 8, 8));

    // Off: Tick and Control never touch the register.
    all(0); b.Control(0, Power(9000)); all(1);
    assert(hw.writes == 0 && b.phase == Phase::Off);

    assert(b.Start(0, 4, {3500, 0}, 8, 24) && !b.Start(0, 4, {3500, 0}, 8, 24));
    tick(0, 1); tick(1, 1);
    b.Control(1, Power(9000)); // no control before Active
    assert(b.phase == Phase::Snapshot && b.cap == 0 && hw.writes == 0);
    tick(2, 2); tick(3, 2);
    assert(b.phase == Phase::Active && b.limits.capTop == 23 && b.limits.releaseRatio == 30);
    // Released: nothing to write.
    all(3);
    assert(hw.writes == 0);
    over(250, 2);
    assert(b.cap == 23 && hw.ratio[0] == 23 && hw.ratio[3] == 23 && b.stepsDown == 1 && hw.writes == 4);
    // Runtime retarget keeps the snapshot.
    assert(b.SetConfig({3000, 0}) && !b.SetConfig({100, 0}) && b.config.coresMw == 3000);
    over(500, 3);
    assert(b.cap == 22 && hw.ratio[1] == 22 && b.stepsDown == 2);
    // Disable: restore originals on every CPU, then off.
    b.Cancel();
    assert(b.phase == Phase::Restore && b.outcome == Outcome::Disabled);
    all(1000);
    assert(b.phase == Phase::Off && hw.ratio[0] == 30 && hw.ratio[3] == 30 && !b.restoreFailed);
    const unsigned settled = hw.writes;
    all(1001); b.Control(1001, Power(9000));
    assert(hw.writes == settled);

    // Never above the per-CPU baseline: a CPU whose firmware asked for 20 stays at 20.
    hw = {};
    hw.ratio[2] = 20;
    assert(b.Start(2000, 4, {3500, 0}, 8, 24));
    all(2001);
    assert(b.limits.capTop == 23);
    over(2002, 2);
    assert(hw.ratio[0] == 23 && hw.ratio[2] == 20 && b.Desired(2) == 20);
    // AC loss (ineligible) restores.
    all(2003, false);
    assert(b.phase == Phase::Off && b.outcome == Outcome::Ineligible && hw.ratio[0] == 30 && hw.ratio[2] == 20);

    // Firmware below the cap range leaves nothing to control.
    hw = {};
    for (auto& r : hw.ratio) r = 8;
    assert(b.Start(3000, 4, {3500, 0}, 8, 24));
    all(3001);
    assert(b.phase == Phase::Off && b.outcome == Outcome::NoRange && hw.writes == 0);

    // Snapshot timeout when a CPU never ticks.
    hw = {};
    assert(b.Start(4000, 4, {3500, 0}, 8, 24));
    tick(0, 4001); tick(0, 7001);
    assert(b.phase == Phase::Off && b.outcome == Outcome::Timeout && hw.writes == 0);

    // Superseded: someone else's value is never overwritten; others are restored.
    hw = {};
    assert(b.Start(8000, 4, {3500, 0}, 8, 24));
    all(8001); over(8002, 2);
    assert(hw.ratio[1] == 23);
    hw.ratio[1] = 27;
    tick(1, 8003);
    assert(b.phase == Phase::Restore && b.outcome == Outcome::Superseded);
    all(8004);
    assert(b.phase == Phase::Off && hw.ratio[1] == 27 && hw.ratio[0] == 30 && b.restoreFailed == 2);

    // Readback mismatch cancels; a stuck register gives up after bounded attempts.
    hw = {};
    assert(b.Start(9000, 4, {3500, 0}, 8, 24));
    all(9001);
    hw.stuck = true;
    over(9002, 2);
    assert(b.phase == Phase::Restore && b.outcome == Outcome::Readback && b.touched == 1);
    all(9003); // the failed write left the firmware value: nothing to undo
    assert(b.phase == Phase::Off && !b.restoreFailed && hw.ratio[0] == 30);
    hw = {};
    assert(b.Start(9500, 4, {3500, 0}, 8, 24));
    all(9501); over(9502, 2);
    hw.stuck = true;
    b.Cancel();
    for (int i = 0; i < 3; ++i) all(9600 + i);
    assert(b.phase == Phase::Off && b.restoreFailed == 0xf && hw.ratio[0] == 23);
    // Cancel during snapshot (no writes) goes straight to off.
    hw = {};
    assert(b.Start(10000, 4, {3500, 0}, 8, 24));
    tick(0, 10001); b.Cancel();
    assert(b.phase == Phase::Off && hw.writes == 0);
}

int main() {
    TestBudgetLaw();
    TestBudgetApply();
    TestEpbTrial();
    TestRatioTrial();
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
