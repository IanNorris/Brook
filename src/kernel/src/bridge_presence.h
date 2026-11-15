#pragma once
#include <stdint.h>

namespace brook {

// Debug-bridge agent presence, as observed from the guest debug listener.
//
// The host bridge (MCP server mode) sends "bridge presence beat INTERVAL"
// periodically while it holds a session for this guest; that beat attaches.
// Other framed requests from the same peer refresh an attached presence but
// never create one, so a one-shot CLI call cannot raise a false alarm 30 s
// later. "bridge presence detach" (explicit hw_detach or a clean MCP exit)
// clears it. A framed request still executing holds the presence fresh: the
// listener is serial, so a heartbeat queued behind a long command could not
// have been answered anyway.
//
// The guest cannot tell why beats stopped (MCP process killed, host asleep,
// cable/link loss on the host side); it reports only how long the host has
// been silent. Guest-side link loss is reported separately by the renderer.
//
// Pure state, no locking: the kernel wrapper serialises access. Time is a
// monotonic millisecond tick supplied by the caller.
enum class BridgePresenceState : uint8_t { Hidden = 0, Connected = 1, Stale = 2 };

struct BridgePresenceView
{
    BridgePresenceState state = BridgePresenceState::Hidden;
    uint32_t peer = 0;        // IPv4, network byte order as stored by the stack
    uint64_t silentMs = 0;    // time since the last beat/refresh (0 while busy)
    uint32_t intervalMs = 0;  // host-advertised heartbeat interval
    bool busy = false;        // a framed request is executing
};

class BridgePresence
{
public:
    static constexpr uint64_t MinStaleMs = 30000;   // heartbeat silence -> warning
    static constexpr uint64_t ClearMs = 600000;     // 10 min silence -> hidden
    static constexpr uint32_t MinIntervalMs = 1000;
    static constexpr uint32_t MaxIntervalMs = 300000;

    void Beat(uint64_t now, uint32_t peer, uint32_t intervalMs)
    {
        if (intervalMs < MinIntervalMs) intervalMs = MinIntervalMs;
        if (intervalMs > MaxIntervalMs) intervalMs = MaxIntervalMs;
        attached_ = true;
        peer_ = peer;
        intervalMs_ = intervalMs;
        lastMs_ = now;
    }

    void Detach() { attached_ = false; }

    // Any framed request from the attached peer counts as host liveness.
    void Activity(uint64_t now, uint32_t peer)
    {
        if (attached_ && peer == peer_ && now > lastMs_) lastMs_ = now;
    }

    void RequestBegin(uint64_t now, uint32_t peer)
    {
        ++inFlight_;
        Activity(now, peer);
    }

    void RequestEnd(uint64_t now, uint32_t peer)
    {
        if (inFlight_) --inFlight_;
        Activity(now, peer);
    }

    uint64_t StaleMs() const
    {
        const uint64_t scaled = 3ull * intervalMs_;
        return scaled > MinStaleMs ? scaled : MinStaleMs;
    }

    // Not const: a presence silent for ClearMs is forgotten here, so a later
    // stray request from that peer cannot resurrect it.
    BridgePresenceView Evaluate(uint64_t now)
    {
        BridgePresenceView view{};
        if (!attached_) return view;
        view.peer = peer_;
        view.intervalMs = intervalMs_;
        if (inFlight_) {
            view.state = BridgePresenceState::Connected;
            view.busy = true;
            return view;
        }
        view.silentMs = now > lastMs_ ? now - lastMs_ : 0;
        if (view.silentMs >= ClearMs) {
            attached_ = false;
            return BridgePresenceView{};
        }
        view.state = view.silentMs >= StaleMs() ? BridgePresenceState::Stale : BridgePresenceState::Connected;
        return view;
    }

    bool Attached() const { return attached_; }

private:
    bool attached_ = false;
    uint32_t peer_ = 0;
    uint32_t intervalMs_ = 10000;
    uint32_t inFlight_ = 0;
    uint64_t lastMs_ = 0;
};

inline const char* BridgePresenceStateName(BridgePresenceState state)
{
    return state == BridgePresenceState::Connected ? "connected" :
           state == BridgePresenceState::Stale ? "stale" : "hidden";
}

} // namespace brook
