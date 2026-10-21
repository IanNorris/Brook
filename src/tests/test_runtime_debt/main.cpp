#include <array>
#include <cstdio>
#include <cstdint>
#include <limits>
#include "../../kernel/src/runtime_accounting.h"

namespace {
unsigned failures;
#define CHECK(x) do { if (!(x)) { if (failures < 20) std::fprintf(stderr, "DEBT line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

// Independent consumed-steal recurrence from Linux v6.12 update_rq_clock_task,
// with IRQ deduction excluded to match Brook's includes-kernel/IRQ contract.
// Unlike the implementation under test, this maintains no service high-water.
struct ConsumedSteal {
    uint64_t wall = 0, published = 0, consumed = 0;
    uint64_t originWall = 0, originSteal = 0, epochCredit = 0;
    uint64_t discardedDebt = 0, discardedEpochs = 0;
    uint64_t episodes = 0, catchups = 0, peak = 0;
    bool valid = false;

    uint64_t debt() const { return valid ? published - consumed : 0; }
    void discard() {
        if (valid) { discardedDebt += debt(); ++discardedEpochs; }
        valid = false;
    }
    uint64_t sample(brook::RuntimeSample s) {
        if (!s.valid || s.stealNs > s.wallNs) { discard(); return 0; }
        if (!valid) {
            wall = originWall = s.wallNs;
            published = consumed = originSteal = s.stealNs;
            epochCredit = 0; valid = true; return 0;
        }
        if (s.wallNs < wall || s.stealNs < published) { discard(); return 0; }
        const auto previousDebt = debt();
        const uint64_t elapsed = s.wallNs - wall;
        const uint64_t pending = s.stealNs - consumed;
        const uint64_t deducted = pending < elapsed ? pending : elapsed;
        consumed += deducted;
        const uint64_t credit = elapsed - deducted;
        epochCredit += credit; wall = s.wallNs; published = s.stealNs;
        if (!previousDebt && debt()) ++episodes;
        if (previousDebt && !debt()) ++catchups;
        if (debt() > peak) peak = debt();
        // Wide signed arithmetic makes the conservation check meaningful even
        // when new published steal temporarily exceeds elapsed epoch wall.
        CHECK(static_cast<__int128>(epochCredit) ==
              (static_cast<__int128>(wall) - originWall) -
              (static_cast<__int128>(published) - originSteal) + debt());
        return credit;
    }
};

brook::RuntimeSample at(uint64_t w, uint64_t s = 0) { return {w, s, true}; }

void nativePair(uint64_t beforeWall, uint64_t wall, uint64_t beforeSteal,
                uint64_t steal, uint64_t deficit) {
    brook::RuntimeAccounting<4, 1> ledger;
    CHECK(ledger.Register(1, 11, 1, 11));
    CHECK(ledger.Register(2, 22, 2, 22));
    ConsumedSteal ref;
    auto previous = at(beforeWall, beforeSteal);
    ref.sample(previous); ledger.Dispatch(0, 1, 11, previous);
    CHECK(ledger.Sample(0, at(wall, steal)) == ref.sample(at(wall, steal)));
    CHECK(ref.debt() == deficit && ledger.Diagnostics().firstClamp.deficitNs == deficit);
    CHECK(ledger.Sample(0, at(wall, steal)) == ref.sample(at(wall, steal)));
    CHECK(ref.debt() == deficit && ledger.Diagnostics().clampedService == 2);
    // Ownership and group change cannot erase debt. Dispatch includes a sample
    // at the old-owner boundary; the new owner gets no invented catch-up credit.
    ref.sample(at(wall, steal)); ledger.Dispatch(0, 2, 22, at(wall, steal));
    wall += deficit / 2;
    CHECK(ledger.Sample(0, at(wall, steal)) == ref.sample(at(wall, steal)));
    wall += deficit - deficit / 2;
    CHECK(ledger.Sample(0, at(wall, steal)) == ref.sample(at(wall, steal)));
    CHECK(ref.debt() == 0);
    CHECK(ledger.Sample(0, at(wall + 1000, steal)) == ref.sample(at(wall + 1000, steal)));
    uint64_t total = 0;
    CHECK(ledger.Read(1, 11, false, &total) && total == 0);
    CHECK(ledger.Read(2, 22, false, &total) && total == 1000);
    CHECK(ledger.Read(1, 11, true, &total) && total == 0);
    CHECK(ledger.Read(2, 22, true, &total) && total == 1000);
}

void nativeOwnershipEpisode() {
    // First complete positive native history: reservation-service-mlfq-a,
    // CPU 3, trigger sample 14509269. Seed at its retained preceding boundary;
    // this does not reconstruct the epoch origin or KVM's publication cause.
    // Keep real PIDs/incarnations and SetOwner boundaries: Dispatch would add
    // samples absent from the trace and incorrectly inflate clamp counts.
    brook::RuntimeAccounting<320,4> ledger;
    for (unsigned pid : {315u,149u,226u}) CHECK(ledger.Register(pid,pid,pid,pid));
    ConsumedSteal ref;
    const auto start=at(307772672519,69874881);
    ref.sample(start);ledger.Dispatch(3,315,315,start);
    auto sample=[&](uint64_t wall,uint64_t steal,uint64_t credit,
                    uint64_t debt,uint64_t clamps) {
        const auto raw=at(wall,steal);
        CHECK(ref.sample(raw)==credit);
        CHECK(ledger.Sample(3,raw)==credit);
        const auto state=ledger.DebtStatus();
        CHECK(ref.debt()==debt && state.knownDebtNs==debt);
        CHECK(state.baselineCpuMask==8 && !state.saturated);
        CHECK(state.episodes==1 && state.catchups==(debt?0u:1u));
        CHECK(state.maxCpuDebtNs==17890 && !state.discardedDebtNs && !state.invalidatedEpochs);
        CHECK(ledger.Diagnostics().clampedService==clamps);
    };
    sample(307772687159,69907411,0,17890,1);
    CHECK(ledger.Diagnostics().firstClamp.previousWallNs==start.wallNs);
    CHECK(ledger.Diagnostics().firstClamp.previousStealNs==start.stealNs);
    CHECK(ledger.Diagnostics().firstClamp.pid==315 && ledger.Diagnostics().firstClamp.incarnation==315);
    CHECK(ledger.Diagnostics().firstClamp.cpu==3 && ledger.Diagnostics().firstClamp.deficitNs==17890);
    ledger.SetOwner(3,149,149);
    CHECK(ledger.DebtStatus().ownerTransitions==1);
    sample(307772694115,69907411,0,10934,2);
    sample(307772694882,69907411,0,10167,3);
    ledger.SetOwner(3,226,226);
    CHECK(ledger.DebtStatus().ownerTransitions==2);
    sample(307772749668,69907411,44619,0,3);
    CHECK(ref.episodes==1 && ref.catchups==1 && ref.peak==17890);
    for (unsigned pid : {315u,149u,226u}) {
        uint64_t value=0;const uint64_t expected=pid==226?44619:0;
        CHECK(ledger.Read(pid,pid,false,&value) && value==expected);
        CHECK(ledger.Read(pid,pid,true,&value) && value==expected);
    }
    const auto& d=ledger.Diagnostics();
    CHECK(d.chargedNs==44619 && d.observedStealNs==32530 && d.idleNs==0);
    CHECK((307772749668ULL-start.wallNs)-(69907411ULL-start.stealNs)==d.chargedNs);
    CHECK(!d.invalidSamples && !d.backwardsWall && !d.backwardsSteal && !d.staleIdentity && !d.saturated);
    // Correct local replay does not clear the native zero-clamp acceptance
    // failure or prove the true task attribution of delayed published steal.
}
}

int main() {
    nativeOwnershipEpisode();
    // Positive retained native adjacent pairs. They reproduce the arithmetic
    // only, not missing native ownership history or the host publication cause.
    nativePair(132683280759, 132683339517, 17197816, 17264256, 7682);
    nativePair(107095032872, 107095055241, 199972129, 199994778, 280);
    nativePair(48059413840, 48059433002, 5222574, 5254982, 13246);
    // Equal timestamps and a large newly published correction are legal model
    // inputs; neither permits negative service or a tolerance-based waiver.
    nativePair(10000, 10000, 100, 300, 200);
    nativePair(1000000000000ULL, 1000000000001ULL, 0, 999999999, 999999998);

    brook::RuntimeAccounting<16, 4> ledger;
    std::array<ConsumedSteal, 4> refs{};
    std::array<uint64_t, 4> wall{}, steal{};
    std::array<unsigned, 4> owner{};
    std::array<uint64_t, 16> threads{}, groups{};
    uint64_t idle = 0, active = 0, debtCrossings = 0;
    for (unsigned pid = 1; pid <= 8; ++pid) {
        const unsigned group = (pid - 1) / 2 * 2 + 1;
        CHECK(ledger.Register(pid, 100 + pid, group, 100 + group));
    }
    for (unsigned cpu = 0; cpu < 4; ++cpu) {
        wall[cpu] = 1000000000000ULL; steal[cpu] = cpu * 1000;
        refs[cpu].sample(at(wall[cpu], steal[cpu]));
        ledger.Dispatch(cpu, 0, 0, at(wall[cpu], steal[cpu]));
    }
    uint32_t rng = 0x7312ab95;
    for (unsigned event = 0; event < 30000; ++event) {
        rng = rng * 1664525u + 1013904223u;
        const unsigned cpu = (rng >> 7) % 4;
        wall[cpu] += (rng >> 10) % 100001;
        steal[cpu] += (rng & 3) ? (rng >> 17) % 100 : (rng >> 8) % 500001;
        auto sample = at(wall[cpu], steal[cpu]);
        // Unknown/backwards boundaries are explicitly discarded epochs, not
        // successful recovery of outstanding accounting debt.
        if (event % 251 == 0) sample.valid = false;
        else if (event % 379 == 0) sample.wallNs = refs[cpu].wall ? refs[cpu].wall - 1 : 0;
        else if (event % 457 == 0 && refs[cpu].published) sample.stealNs = refs[cpu].published - 1;
        const uint64_t credit = refs[cpu].sample(sample);
        const uint64_t charged = ledger.Sample(cpu, sample);
        CHECK(charged == (owner[cpu] ? credit : 0));
        if (owner[cpu]) {
            threads[owner[cpu]] += credit;
            groups[(owner[cpu] - 1) / 2 * 2 + 1] += credit; active += credit;
        } else idle += credit;
        if ((rng >> 20) % 3 == 0) {
            const unsigned next = (rng >> 23) % 3;
            const unsigned selected = next ? cpu * 2 + next : 0;
            if (refs[cpu].debt() && selected != owner[cpu]) ++debtCrossings;
            owner[cpu] = selected;
            ledger.SetOwner(cpu, owner[cpu], owner[cpu] ? 100 + owner[cpu] : 0);
        }
        const auto debt = ledger.DebtStatus();
        uint64_t known = 0, mask = 0, lost = 0, epochs = 0, episodes = 0, catchups = 0, peak = 0;
        for (unsigned c = 0; c < 4; ++c) {
            const auto& r = refs[c]; known += r.debt();
            if (r.valid) mask |= uint64_t(1) << c;
            lost += r.discardedDebt; epochs += r.discardedEpochs;
            episodes += r.episodes; catchups += r.catchups;
            if (r.peak > peak) peak = r.peak;
        }
        CHECK(debt.knownDebtNs == known && debt.baselineCpuMask == mask);
        CHECK(debt.discardedDebtNs == lost && debt.invalidatedEpochs == epochs);
        CHECK(debt.episodes == episodes && debt.catchups == catchups);
        CHECK(debt.maxCpuDebtNs == peak && debt.ownerTransitions == debtCrossings && !debt.saturated);
        CHECK(ledger.Diagnostics().chargedNs == active && ledger.Diagnostics().idleNs == idle);
        for (unsigned pid = 1; pid <= 8; ++pid) {
            uint64_t value = 0;
            CHECK(ledger.Read(pid, 100 + pid, false, &value) && value == threads[pid]);
            const unsigned group = (pid - 1) / 2 * 2 + 1;
            CHECK(ledger.Read(group, 100 + group, true, &value) && value == groups[group]);
        }
    }
    uint64_t lost = 0, epochs = 0;
    for (const auto& ref : refs) { lost += ref.discardedDebt; epochs += ref.discardedEpochs; }
    CHECK(lost > 0 && epochs > 0 && debtCrossings > 0 && ledger.Diagnostics().clampedService > 0);
    CHECK(ledger.Diagnostics().invalidSamples > 0 && ledger.Diagnostics().backwardsWall > 0 && ledger.Diagnostics().backwardsSteal > 0);
    CHECK(!ledger.Diagnostics().staleIdentity && !ledger.Diagnostics().saturated);

    // Attribution exposure counts actual owner identity changes, including
    // PID reuse and idle, without erasing debt or counting idempotent writes.
    {
        brook::RuntimeAccounting<4,2> l;
        l.Dispatch(0,1,11,at(1000));l.Sample(0,at(1000,100));
        l.SetOwner(0,1,11);CHECK(l.DebtStatus().ownerTransitions==0);
        l.SetOwner(0,1,12);l.SetOwner(0,0,0);
        CHECK(l.DebtStatus().ownerTransitions==2 && l.DebtStatus().knownDebtNs==100);
        l.Sample(0,at(1050,100));CHECK(l.DebtStatus().knownDebtNs==50);
        l.Sample(0,at(1100,100));
        CHECK(l.DebtStatus().catchups==1 && !l.DebtStatus().knownDebtNs);
        l.Sample(0,at(1100,150));l.Sample(0,{});l.Sample(0,{});
        auto d=l.DebtStatus();
        CHECK(d.episodes==2 && d.catchups==1 && d.discardedDebtNs==50);
        CHECK(d.invalidatedEpochs==1 && d.baselineCpuMask==0);
        l.Sample(0,at(2000,150));d=l.DebtStatus();
        CHECK(d.baselineCpuMask==1 && d.catchups==1 && !d.knownDebtNs);
        l.Sample(0,at(1999,150));l.Sample(0,{});
        CHECK(l.DebtStatus().invalidatedEpochs==2); // zero-debt lost epoch too
    }
    // An overflowing diagnostic sum/cumulative counter is explicit and cannot
    // change charges. Snapshot sum saturation is not a persistent debt value.
    {
        constexpr uint64_t top=~uint64_t(0);
        brook::RuntimeAccounting<2,2> l;
        for(unsigned c=0;c<2;++c){l.Sample(c,at(top));l.Sample(c,at(top,top));}
        auto d=l.DebtStatus();CHECK(d.knownDebtNs==top && d.saturated && d.maxCpuDebtNs==top);
        l.Sample(1,{});d=l.DebtStatus();
        CHECK(d.knownDebtNs==top && !d.saturated && d.discardedDebtNs==top);
        l.Sample(0,{});d=l.DebtStatus();
        CHECK(d.saturated && d.discardedDebtNs==top && !d.knownDebtNs);
        CHECK(d.invalidatedEpochs==2 && d.episodes==2 && !d.catchups);
        CHECK(!l.Diagnostics().chargedNs && !l.Diagnostics().idleNs);
    }

    // The reference comparison deliberately stays in Linux's signed-delta
    // domain, while checking subtraction near its representable upper edge.
    const uint64_t end = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    nativePair(end - 100000, end - 99000, end - 500000, end - 498000, 1000);
    std::printf("RUNTIME_DEBT %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
