#include "input.h"
#include "serial.h"
#include "scheduler.h"
#include "process.h"

namespace brook {

// ---------------------------------------------------------------------------
// Input waiter list — processes blocked waiting for events
// ---------------------------------------------------------------------------

static constexpr uint32_t WAITER_MAX = INPUT_MAX_WAITERS;
static Process* g_waiters[WAITER_MAX];
static volatile uint32_t g_waiterCount = 0;

void InputAddWaiter(Process* proc)
{
    uint32_t n = g_waiterCount;
    if (n >= WAITER_MAX) return;
    // Avoid duplicates
    for (uint32_t i = 0; i < n; ++i)
        if (g_waiters[i] == proc) return;
    g_waiters[n] = proc;
    __atomic_store_n(&g_waiterCount, n + 1, __ATOMIC_RELEASE);
}

void InputRemoveWaiter(Process* proc)
{
    uint32_t n = g_waiterCount;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (g_waiters[i] == proc)
        {
            g_waiters[i] = g_waiters[n - 1];
            g_waiters[n - 1] = nullptr;
            __atomic_store_n(&g_waiterCount, n - 1, __ATOMIC_RELEASE);
            return;
        }
    }
}

void InputWakeWaiters()
{
    uint32_t n = __atomic_load_n(&g_waiterCount, __ATOMIC_ACQUIRE);
    if (n == 0) return;

    // Snapshot and clear the waiter list, then unblock each process.
    // Set pendingWakeup FIRST to handle the race where the process hasn't
    // called SchedulerBlock yet — SchedulerBlock checks pendingWakeup and
    // skips blocking if set.
    Process* snap[WAITER_MAX];
    for (uint32_t i = 0; i < n; ++i)
    {
        snap[i] = g_waiters[i];
        g_waiters[i] = nullptr;
        __atomic_store_n(&snap[i]->pendingWakeup, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_waiterCount, 0u, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < n; ++i)
        SchedulerUnblock(snap[i]);
}

// ---------------------------------------------------------------------------
// Input device registry
// ---------------------------------------------------------------------------

static InputDevice* g_inputDevices[INPUT_MAX_DEVICES];
static uint32_t     g_inputDeviceCount = 0;
static bool         g_inputReady = false;

void InputInit()
{
    g_inputDeviceCount = 0;
    g_inputReady = true;
    SerialPuts("INPUT: subsystem initialised\n");
}

bool InputRegister(InputDevice* dev)
{
    if (!dev || !dev->ops)
    {
        SerialPuts("INPUT: null device or ops\n");
        return false;
    }

    if (g_inputDeviceCount >= INPUT_MAX_DEVICES)
    {
        SerialPuts("INPUT: device table full\n");
        return false;
    }

    // Initialise ring buffer.
    dev->head = 0;
    dev->tail = 0;

    g_inputDevices[g_inputDeviceCount++] = dev;
    SerialPrintf("INPUT: registered '%s' (%u total)\n",
                 dev->ops->name ? dev->ops->name : "?",
                 g_inputDeviceCount);
    return true;
}

bool InputPollEvent(InputEvent* out)
{
    // Round-robin across devices so no single device starves others.
    for (uint32_t i = 0; i < g_inputDeviceCount; ++i)
    {
        InputDevice* dev = g_inputDevices[i];

        // If the driver has a poll callback, call it first.
        if (dev->ops->poll)
            dev->ops->poll(dev);

        if (InputDevicePop(dev, out))
            return true;
    }
    return false;
}

InputEvent InputWaitEvent()
{
    InputEvent ev;
    while (!InputPollEvent(&ev))
        __asm__ volatile("hlt"); // sleep until next interrupt
    return ev;
}

bool InputHasEvents()
{
    for (uint32_t i = 0; i < g_inputDeviceCount; ++i)
    {
        if (__atomic_load_n(&g_inputDevices[i]->head, __ATOMIC_ACQUIRE) !=
            __atomic_load_n(&g_inputDevices[i]->tail, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

} // namespace brook
