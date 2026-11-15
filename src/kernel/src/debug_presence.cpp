#include "debug_presence.h"
#include "bridge_reply.h"
#include "spinlock.h"
#include "apic.h"
#include "net.h"
#include "serial.h"

namespace brook {
namespace {
BridgePresence g_presence;
SpinLock g_presenceLock{};
// Peer of the framed request being dispatched. Only the single debug_tcp
// listener thread brackets requests, so at most one is current.
uint32_t g_currentPeer = 0;
using Reply = BridgeReplyBuffer<DebugChannelSend>;

bool Equals(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void SendStatus(const BridgePresenceView& view)
{
    Reply r;
    r.text("{\"presence_api\":1,\"state\":");
    r.quoted(BridgePresenceStateName(view.state));
    r.text(",\"attached\":");
    r.text(view.state != BridgePresenceState::Hidden ? "true" : "false");
    r.text(",\"busy\":");
    r.text(view.busy ? "true" : "false");
    r.text(",");r.field("silent_ms", view.silentMs);
    r.text(",");r.field("interval_ms", view.intervalMs);
    r.text(",");r.field("clear_after_ms", BridgePresence::ClearMs);
    r.text(",\"peer\":\"");
    const uint8_t* o = reinterpret_cast<const uint8_t*>(&view.peer);
    for (int i = 0; i < 4; ++i) { r.number(o[i]); if (i < 3) r.text("."); }
    r.text("\"}\n");
    r.send();
}
}

void DebugPresenceRequestBegin(uint32_t peer)
{
    const uint64_t now = ApicTickCount();
    SpinLockAcquire(&g_presenceLock);
    g_currentPeer = peer;
    g_presence.RequestBegin(now, peer);
    SpinLockRelease(&g_presenceLock);
}

void DebugPresenceRequestEnd()
{
    const uint64_t now = ApicTickCount();
    SpinLockAcquire(&g_presenceLock);
    g_presence.RequestEnd(now, g_currentPeer);
    g_currentPeer = 0;
    SpinLockRelease(&g_presenceLock);
}

BridgePresenceView DebugPresenceSnapshot(uint64_t now)
{
    SpinLockAcquire(&g_presenceLock);
    const BridgePresenceView view = g_presence.Evaluate(now);
    SpinLockRelease(&g_presenceLock);
    return view;
}

bool DebugPresenceHandleCommand(const char* command)
{
    constexpr const char prefix[] = "bridge presence";
    for (unsigned i = 0; i + 1 < sizeof(prefix); ++i) if (command[i] != prefix[i]) return false;
    const char* rest = command + sizeof(prefix) - 1;
    if (*rest && *rest != ' ') return false;
    const uint64_t now = ApicTickCount();
    if (*rest) {
        ++rest;
        if (Equals(rest, "detach")) {
            SpinLockAcquire(&g_presenceLock);
            const bool was = g_presence.Attached();
            g_presence.Detach();
            SpinLockRelease(&g_presenceLock);
            if (was) SerialPuts("bridge: agent presence detached\n");
        } else if (rest[0] == 'b' && rest[1] == 'e' && rest[2] == 'a' && rest[3] == 't' && rest[4] == ' ') {
            uint32_t seconds = 0, digits = 0;
            for (const char* p = rest + 5; *p; ++p, ++digits) {
                if (*p < '0' || *p > '9' || digits >= 4) { seconds = 0; digits = 0; break; }
                seconds = seconds * 10 + static_cast<uint32_t>(*p - '0');
            }
            if (!digits || !seconds) {
                DebugChannelSend("{\"presence_error\":\"Expected heartbeat interval in seconds\"}\n");
                return true;
            }
            SpinLockAcquire(&g_presenceLock);
            const bool fresh = !g_presence.Attached();
            const uint32_t peer = g_currentPeer;
            g_presence.Beat(now, peer, seconds * 1000);
            SpinLockRelease(&g_presenceLock);
            if (fresh) {
                const uint8_t* o = reinterpret_cast<const uint8_t*>(&peer);
                SerialPrintf("bridge: agent presence attached from %u.%u.%u.%u interval=%us\n",
                             o[0], o[1], o[2], o[3], seconds);
            }
        } else {
            DebugChannelSend("{\"presence_error\":\"Unknown presence command\"}\n");
            return true;
        }
    }
    SendStatus(DebugPresenceSnapshot(now));
    return true;
}
}
