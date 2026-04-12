#include "device.h"
#include "serial.h"

namespace brook {

// ---- Registry storage ----

static Device* g_devices[DEVICE_MAX];
static uint32_t g_deviceCount = 0;

// ---- String helpers (no libc) ----

static bool StrEq(const char* a, const char* b)
{
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

// ---- Public API ----

bool DeviceRegister(Device* dev)
{
    if (!dev || !dev->name || !dev->ops)
    {
        SerialPuts("DeviceRegister: null device, name, or ops\n");
        return false;
    }

    if (g_deviceCount >= DEVICE_MAX)
    {
        SerialPuts("DeviceRegister: device table full\n");
        return false;
    }

    // Check for duplicate name.
    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (StrEq(g_devices[i]->name, dev->name))
        {
            SerialPrintf("DeviceRegister: duplicate name '%s'\n", dev->name);
            return false;
        }
    }

    g_devices[g_deviceCount++] = dev;
    SerialPrintf("DEV: registered '%s' (type %u)\n",
                 dev->name, static_cast<unsigned>(dev->type));
    return true;
}

bool DeviceUnregister(Device* dev)
{
    if (!dev) return false;

    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (g_devices[i] == dev)
        {
            SerialPrintf("DEV: unregistered '%s'\n", dev->name);

            // Shift remaining entries down.
            for (uint32_t j = i; j + 1 < g_deviceCount; ++j)
                g_devices[j] = g_devices[j + 1];
            g_devices[--g_deviceCount] = nullptr;
            return true;
        }
    }
    return false;
}

Device* DeviceFind(const char* name)
{
    if (!name) return nullptr;
    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (StrEq(g_devices[i]->name, name))
            return g_devices[i];
    }
    return nullptr;
}

void DeviceIterate(DeviceType type, bool (*cb)(Device* dev, void* ctx), void* ctx)
{
    if (!cb) return;
    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (g_devices[i]->type == type)
        {
            if (!cb(g_devices[i], ctx)) return;
        }
    }
}

} // namespace brook
