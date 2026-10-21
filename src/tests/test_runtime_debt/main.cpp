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
        const uint64_t elapsed = s.wallNs - wall;
        const uint64_t pending = s.stealNs - consumed;
        const uint64_t deducted = pending < elapsed ? pending : elapsed;
        consumed += deducted;
        const uint64_t credit = elapsed - deducted;
        epochCredit += credit; wall = s.wallNs; published = s.stealNs;
        // Wide signed arithmetic makes the conservation check meaningful even
        // when new published steal temporarily exceeds elapsed epoch wall.
        CHECK(static_cast<__int128>(epochCredit) ==
              (static_cast<__int128>(wall) - originWall) -
              (static_cast<__int128>(published) - originSteal) + debt());
        return credit;
    }
};

brook::RuntimeSample at(uint64_t w, uint64_t s) { return {w, s, true}; }

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
}

int main() {
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

    // The reference comparison deliberately stays in Linux's signed-delta
    // domain, while checking subtraction near its representable upper edge.
    const uint64_t end = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    nativePair(end - 100000, end - 99000, end - 500000, end - 498000, 1000);
    std::printf("RUNTIME_DEBT %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
