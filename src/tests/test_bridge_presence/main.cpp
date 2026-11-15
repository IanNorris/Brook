#include "../../kernel/src/bridge_presence.h"
#include <cassert>
using namespace brook;

int main() {
    constexpr uint32_t host = 0x0b02000a, other = 0x0c02000a;
    using S = BridgePresenceState;

    // Requests without a heartbeat (CLI one-shot calls) never show anything.
    BridgePresence p;
    p.RequestBegin(1000, host);
    assert(p.Evaluate(1500).state == S::Hidden);
    p.RequestEnd(2000, host);
    assert(p.Evaluate(2000).state == S::Hidden && !p.Attached());

    // A heartbeat attaches; fresh until the stale threshold.
    p.Beat(10000, host, 10000);
    BridgePresenceView v = p.Evaluate(10000);
    assert(v.state == S::Connected && v.peer == host && v.intervalMs == 10000 && v.silentMs == 0);
    assert(p.Evaluate(10000 + 29999).state == S::Connected);
    v = p.Evaluate(10000 + 30000);
    assert(v.state == S::Stale && v.silentMs == 30000);

    // Another heartbeat or any framed request from the same peer recovers it.
    p.Beat(45000, host, 10000);
    assert(p.Evaluate(46000).state == S::Connected);
    p.RequestBegin(70000, host);
    p.RequestEnd(70100, host);
    assert(p.Evaluate(70100 + 29000).state == S::Connected);
    // A different peer's request does not stand in for the agent.
    p.RequestBegin(90000, other);
    p.RequestEnd(90001, other);
    assert(p.Evaluate(70100 + 30000).state == S::Stale);

    // A long-running request holds the serial listener: busy is not stale.
    p.RequestBegin(110000, other);
    v = p.Evaluate(200000);
    assert(v.state == S::Connected && v.busy && v.silentMs == 0);
    p.RequestEnd(200000, other);
    assert(p.Evaluate(200001).state == S::Stale);

    // Stale threshold scales with a long advertised interval (3x), min 30 s.
    p.Beat(300000, host, 20000);
    assert(p.StaleMs() == 60000);
    assert(p.Evaluate(300000 + 59999).state == S::Connected);
    assert(p.Evaluate(300000 + 60000).state == S::Stale);
    p.Beat(400000, host, 1);  // clamped to 1 s; threshold floor stays 30 s
    assert(p.StaleMs() == BridgePresence::MinStaleMs);
    p.Beat(400000, host, 10000000);
    assert(p.StaleMs() == 3ull * BridgePresence::MaxIntervalMs);
    p.Beat(400000, host, 10000);

    // Ten minutes of silence clears and forgets; stray requests do not revive.
    assert(p.Evaluate(400000 + BridgePresence::ClearMs - 1).state == S::Stale);
    assert(p.Evaluate(400000 + BridgePresence::ClearMs).state == S::Hidden);
    assert(!p.Attached());
    p.RequestBegin(400000 + BridgePresence::ClearMs + 5, host);
    p.RequestEnd(400000 + BridgePresence::ClearMs + 6, host);
    assert(p.Evaluate(400000 + BridgePresence::ClearMs + 7).state == S::Hidden);

    // Explicit detach clears immediately, even while stale or busy.
    p.Beat(2000000, host, 10000);
    assert(p.Evaluate(2050000).state == S::Stale);
    p.Detach();
    assert(p.Evaluate(2050001).state == S::Hidden);
    p.Beat(2100000, host, 10000);
    p.RequestBegin(2100001, host);
    p.Detach();
    assert(p.Evaluate(2100002).state == S::Hidden);
    p.RequestEnd(2100003, host);
    assert(p.Evaluate(2100004).state == S::Hidden);

    // A clock that appears to step backwards is treated as no silence.
    p.Beat(3000000, host, 10000);
    v = p.Evaluate(2999000);
    assert(v.state == S::Connected && v.silentMs == 0);

    assert(BridgePresenceStateName(S::Connected)[0] == 'c');
    assert(BridgePresenceStateName(S::Stale)[0] == 's');
    assert(BridgePresenceStateName(S::Hidden)[0] == 'h');
    return 0;
}
