#include <cassert>
#include <cstdint>

enum class ProcessState { Running, Terminated };
struct Process { ProcessState state; bool terminal; };
struct TerminalGeometry {};
struct TestWindow {
    Process* proc;
    bool focusable;
    bool vfb;
    bool cooperativeClose;
};

static constexpr unsigned WM_MAX_WINDOWS=1;
static constexpr uint8_t WM_EVT_CLOSE_REQUESTED=1;
static TestWindow g_windows[WM_MAX_WINDOWS];
static unsigned closeEvents;
static bool queueHasRoom=true;
static int signalSent;

static bool WmPushWmEvent(TestWindow* window, uint8_t type, int16_t, int16_t)
{
    assert(window==&g_windows[0] && type==WM_EVT_CLOSE_REQUESTED);
    if (!queueHasRoom) return false;
    ++closeEvents;
    return true;
}

static bool TerminalSnapshotGeometry(Process* process, TerminalGeometry*) { return process->terminal; }
static void ProcessSendSignal(Process*, int signal) { signalSent=signal; }

#include "../../kernel/src/wm_request_close.inc"

int main()
{
    Process process{ProcessState::Running, false};
    uint32_t method=0;

    g_windows[0]={&process, true, false, true};
    assert(RequestCloseLocked(0, &method));
    assert(method==1 && closeEvents==1 && !signalSent);
    queueHasRoom=false;
    assert(!RequestCloseLocked(0, &method));
    assert(closeEvents==1 && !signalSent);

    queueHasRoom=true;
    g_windows[0]={&process, true, true, false};
    assert(RequestCloseLocked(0, &method));
    assert(method==1 && closeEvents==2 && !signalSent);

    g_windows[0]={&process, true, false, false};
    process.terminal=true;
    assert(RequestCloseLocked(0, &method));
    assert(method==2 && signalSent==1);
    process.terminal=false;
    assert(RequestCloseLocked(0, &method));
    assert(method==3 && signalSent==15);
    assert(closeEvents==2);
}
