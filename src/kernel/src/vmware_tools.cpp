// VMware Tools TCLO/RPCI guest service. Protocol in vmware_rpc.h; this file
// owns the single toolbox session, its polling thread and kernel policy.
#include "vmware_tools.h"
#include "apic.h"
#include "net.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "shutdown.h"
#include "spinlock.h"
#include "vmware_rpc_port.h"

namespace brook {
namespace {

constexpr uint32_t PollMs = 100;
constexpr uint32_t ReconnectMs = 2000;

vmware::RpcPort g_port;
vmware::ToolsRpc<vmware::RpcPort> g_rpc{g_port};
SpinLock g_lock{};
bool g_running = false;
// Display limits published by the driver, applied by the thread.
uint32_t g_limitWidth = 0, g_limitHeight = 0;
bool g_limitChanged = false;

bool AcceptPower(vmware::PowerOp op)
{
    const auto caps = ShutdownGetPowerCapabilities();
    const bool available = caps.gracefulQuiescence &&
        (op == vmware::PowerOp::Halt ? caps.powerOff : caps.reset);
    return available && ShutdownLaunchAllowed();
}

const char* PowerName(vmware::PowerOp op)
{
    return op == vmware::PowerOp::Halt ? "Shut Down Guest" : "Restart Guest";
}

// One bounded tick: at most a reopen, an announcement, an IP report and one
// TCLO exchange. Returns the delay before the next tick.
uint32_t Tick(bool& announced, uint32_t& reportedIp)
{
    using vmware::RpcResult;
    if (!SpinLockTryAcquire(&g_lock)) return PollMs;
    const uint64_t now = ApicTickCount();
    if (!g_rpc.IsOpen()) {
        if (!g_rpc.Open()) {
            SpinLockRelease(&g_lock);
            return ReconnectMs;
        }
        announced = false;
        reportedIp = 0;
    }
    if (__atomic_exchange_n(&g_limitChanged, false, __ATOMIC_ACQ_REL)) {
        g_rpc.SetResolutionLimit({__atomic_load_n(&g_limitWidth, __ATOMIC_ACQUIRE),
                                  __atomic_load_n(&g_limitHeight, __ATOMIC_ACQUIRE)});
        announced = false;
    }
    bool ok = true;
    if (!announced) {
        static bool logged = false;
        ok = g_rpc.Announce() && (logged || g_rpc.Log("Brook guest tools service started"));
        if (ok && !logged) {
            logged = true;
            SerialPrintf("vmware-tools: toolbox registered (guest-managed version)\n");
        }
        announced = ok;
    }
    const NetIf* nif = NetGetIf();
    const uint32_t ip = nif ? __atomic_load_n(&nif->ipAddr, __ATOMIC_ACQUIRE) : 0;
    if (ok && ip && (ip != reportedIp || g_rpc.TakeIpRequest())) {
        ok = g_rpc.SendGuestIp(ip);
        if (ok) {
            reportedIp = ip;
            SerialPrintf("vmware-tools: guest IP %u.%u.%u.%u reported\n",
                         ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
        }
    }
    vmware::RpcPoll poll{RpcResult::TransportError};
    if (ok) poll = g_rpc.Poll(now);
    else g_rpc.Close();
    if (poll.result == RpcResult::WaitingForApply) {
        // Enforce the resize deadline even if the display stops polling.
        vmware::ResizeMode ignored{};
        (void)g_rpc.PendingMode(now, ignored);
    }
    if (poll.result == RpcResult::PowerRequested) {
        const bool accept = AcceptPower(poll.power);
        g_rpc.AnswerPower(accept);
        SerialPrintf("vmware-tools: host %s %s\n", PowerName(poll.power),
                     accept ? "accepted" : "declined (unavailable or busy)");
    }
    SpinLockRelease(&g_lock);
    if (poll.committed != vmware::PowerOp::None) {
        SerialPrintf("vmware-tools: starting checked %s\n",
                     poll.committed == vmware::PowerOp::Halt ? "shutdown" : "restart");
        ShutdownDesktopRequest(poll.committed == vmware::PowerOp::Reboot);
    }
    if (poll.result == RpcResult::TransportError) return ReconnectMs;
    // Answer an accepted power request promptly.
    return poll.result == RpcResult::PowerRequested || poll.committed != vmware::PowerOp::None ? 1 : PollMs;
}

void Worker(void*)
{
    bool announced = false;
    uint32_t reportedIp = 0;
    for (;;) SchedulerSleepMs(Tick(announced, reportedIp));
}

} // namespace

void VmwareToolsStart()
{
    // Strict no-op on physical machines and other hypervisors: the port
    // adapter refuses all I/O unless exact VMware CPUID detection succeeds.
    if (!g_port.Probe()) return;
    Process* worker = KernelThreadCreate("vmware-tools", Worker, nullptr, 3);
    if (!worker) {
        SerialPuts("vmware-tools: thread creation failed\n");
        return;
    }
    __atomic_store_n(&g_running, true, __ATOMIC_RELEASE);
    SchedulerAddProcess(worker);
    SerialPuts("vmware-tools: toolbox service started\n");
}

} // namespace brook

using namespace brook;

extern "C" bool VmwareToolsRegisterResolution(uint32_t maxWidth, uint32_t maxHeight)
{
    if (!__atomic_load_n(&g_running, __ATOMIC_ACQUIRE) || !maxWidth || !maxHeight) return false;
    __atomic_store_n(&g_limitWidth, maxWidth, __ATOMIC_RELEASE);
    __atomic_store_n(&g_limitHeight, maxHeight, __ATOMIC_RELEASE);
    __atomic_store_n(&g_limitChanged, true, __ATOMIC_RELEASE);
    return true;
}

extern "C" bool VmwareToolsTakeResolution(uint32_t* width, uint32_t* height)
{
    if (!__atomic_load_n(&g_running, __ATOMIC_ACQUIRE) || !SpinLockTryAcquire(&g_lock)) return false;
    vmware::ResizeMode mode{};
    const bool pending = g_rpc.PendingMode(ApicTickCount(), mode);
    SpinLockRelease(&g_lock);
    if (pending) { *width = mode.width; *height = mode.height; }
    return pending;
}

extern "C" void VmwareToolsFinishResolution(bool applied)
{
    if (!__atomic_load_n(&g_running, __ATOMIC_ACQUIRE)) return;
    SpinLockAcquire(&g_lock);
    g_rpc.CompletePending(applied ? vmware::ResizeApplyResult::Applied
                                  : vmware::ResizeApplyResult::Invalid);
    SpinLockRelease(&g_lock);
}
