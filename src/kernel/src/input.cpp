#include "input.h"
#include "serial.h"

namespace brook {

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
        if (g_inputDevices[i]->head != g_inputDevices[i]->tail)
            return true;
    }
    return false;
}

} // namespace brook
