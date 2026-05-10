#include "device.h"
#include "serial.h"
#include "spinlock.h"

namespace brook {

// ---- Registry storage ----

static constexpr uint32_t DEVICE_CANARY = 0xDE41CE42;
static uint32_t g_deviceCanaryPre = DEVICE_CANARY;
static Device* g_devices[DEVICE_MAX];
static uint32_t g_deviceCount = 0;
static uint32_t g_deviceCanaryPost = DEVICE_CANARY;
static SpinLock g_deviceLock;

// Rolling checksum of the g_devices[] array + count.  Updated on every
// Register/Unregister.  Verified periodically from the timer tick to
// detect silent corruption of the BSS region.
static uint64_t g_deviceChecksum = 0;

static uint64_t ComputeDeviceChecksum()
{
    // Simple xor-rotate hash over the pointer array and count.
    uint64_t h = 0x12345678DEADBEEFULL ^ static_cast<uint64_t>(g_deviceCount);
    for (uint32_t i = 0; i < DEVICE_MAX; i++)
    {
        uint64_t v = reinterpret_cast<uint64_t>(g_devices[i]);
        h ^= v;
        h = (h << 7) | (h >> 57); // rotate left 7
    }
    return h;
}

static void UpdateDeviceChecksum()
{
    g_deviceChecksum = ComputeDeviceChecksum();
}

// Returns true if a Device* looks like a valid kernel heap or image pointer.
static bool DevicePtrLooksValid(const Device* d)
{
    uint64_t v = reinterpret_cast<uint64_t>(d);
    if (v == 0) return false;
    // Kernel image: 0xFFFFFFFF80000000 .. 0xFFFFFFFF80FFFFFF
    bool inImage = (v >= 0xFFFFFFFF80000000ULL && v < 0xFFFFFFFF81000000ULL);
    // Kernel heap:  0xFFFFC08000000000 .. 0xFFFFC0FF00000000
    bool inHeap  = (v >= 0xFFFFC08000000000ULL && v < 0xFFFFC0FF00000000ULL);
    return inImage || inHeap;
}

uint32_t DeviceCountRaw() { return g_deviceCount; }

bool DeviceRegistryCorrupted()
{
    return g_deviceCanaryPre != DEVICE_CANARY ||
           g_deviceCanaryPost != DEVICE_CANARY ||
           g_deviceCount > DEVICE_MAX;
}

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

    uint64_t flags = SpinLockAcquire(&g_deviceLock);

    if (g_deviceCount >= DEVICE_MAX)
    {
        SpinLockRelease(&g_deviceLock, flags);
        SerialPuts("DeviceRegister: device table full\n");
        return false;
    }

    // Check for duplicate name.
    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (StrEq(g_devices[i]->name, dev->name))
        {
            SpinLockRelease(&g_deviceLock, flags);
            SerialPrintf("DeviceRegister: duplicate name '%s'\n", dev->name);
            return false;
        }
    }

    g_devices[g_deviceCount++] = dev;
    UpdateDeviceChecksum();
    SpinLockRelease(&g_deviceLock, flags);

    SerialPrintf("DEV: registered '%s' (type %u)\n",
                 dev->name, static_cast<unsigned>(dev->type));
    return true;
}

bool DeviceUnregister(Device* dev)
{
    if (!dev) return false;

    uint64_t flags = SpinLockAcquire(&g_deviceLock);

    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (g_devices[i] == dev)
        {
            // Shift remaining entries down.
            for (uint32_t j = i; j + 1 < g_deviceCount; ++j)
                g_devices[j] = g_devices[j + 1];
            g_devices[--g_deviceCount] = nullptr;
            UpdateDeviceChecksum();
            SpinLockRelease(&g_deviceLock, flags);

            SerialPrintf("DEV: unregistered '%s'\n", dev->name);
            return true;
        }
    }

    SpinLockRelease(&g_deviceLock, flags);
    return false;
}

Device* DeviceFind(const char* name)
{
    if (!name) return nullptr;
    uint64_t flags = SpinLockAcquire(&g_deviceLock);
    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (StrEq(g_devices[i]->name, name))
        {
            Device* result = g_devices[i];
            SpinLockRelease(&g_deviceLock, flags);
            return result;
        }
    }
    SpinLockRelease(&g_deviceLock, flags);
    return nullptr;
}

bool DeviceIsRegistered(Device* dev)
{
    if (!dev) return false;
    uint64_t flags = SpinLockAcquire(&g_deviceLock);
    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (g_devices[i] == dev)
        {
            SpinLockRelease(&g_deviceLock, flags);
            return true;
        }
    }
    SpinLockRelease(&g_deviceLock, flags);
    return false;
}

void DeviceDumpRegistry()
{
    SerialPrintf("DEV_DUMP: count=%u canaryPre=0x%08x canaryPost=0x%08x\n",
                 g_deviceCount, g_deviceCanaryPre, g_deviceCanaryPost);
    uint32_t limit = g_deviceCount;
    if (limit > 16) limit = 16;
    for (uint32_t i = 0; i < limit; ++i)
    {
        Device* d = g_devices[i];
        if (!DevicePtrLooksValid(d))
        {
            SerialPrintf("  [%u] %p (INVALID PTR — skipping dereference)\n", i, d);
            continue;
        }
        // Even with a valid-looking pointer, the name might be garbage.
        // Print at most 32 chars, checking each byte.
        const char* name = d->name;
        char safeName[33];
        bool nameOk = false;
        if (name && DevicePtrLooksValid(reinterpret_cast<const Device*>(
                reinterpret_cast<uint64_t>(name) & ~7ULL)))
        {
            // Pointer is in a valid kernel region — cautiously copy
            for (int c = 0; c < 32; c++)
            {
                char ch = name[c];
                if (ch == '\0') { safeName[c] = '\0'; nameOk = true; break; }
                safeName[c] = (ch >= 0x20 && ch < 0x7F) ? ch : '?';
            }
            safeName[32] = '\0';
            if (!nameOk) nameOk = true; // truncated but printed
        }
        if (!nameOk)
        {
            safeName[0] = '?'; safeName[1] = '\0';
        }
        SerialPrintf("  [%u] %p name='%s' type=%u ops=%p\n",
                     i, d, safeName,
                     static_cast<unsigned>(d->type),
                     d->ops);
    }
}

bool DeviceCheckIntegrity()
{
    uint64_t flags = SpinLockAcquire(&g_deviceLock);
    bool ok = true;
    if (g_deviceCanaryPre != DEVICE_CANARY ||
        g_deviceCanaryPost != DEVICE_CANARY)
    {
        SerialPrintf("DEV_INTEGRITY: canary mismatch! pre=0x%08x post=0x%08x\n",
                     g_deviceCanaryPre, g_deviceCanaryPost);
        ok = false;
    }
    else if (g_deviceCount > DEVICE_MAX)
    {
        SerialPrintf("DEV_INTEGRITY: count=%u exceeds max=%u\n",
                     g_deviceCount, DEVICE_MAX);
        ok = false;
    }
    else
    {
        uint64_t expected = g_deviceChecksum;
        uint64_t actual   = ComputeDeviceChecksum();
        if (expected != actual)
        {
            SerialPrintf("DEV_INTEGRITY: CHECKSUM MISMATCH! expected=0x%lx actual=0x%lx count=%u\n",
                         expected, actual, g_deviceCount);
            for (uint32_t i = 0; i < g_deviceCount && i < 16; i++)
            {
                SerialPrintf("  g_devices[%u] = %p%s\n", i, g_devices[i],
                             DevicePtrLooksValid(g_devices[i]) ? "" : " (INVALID)");
            }
            ok = false;
        }
    }
    SpinLockRelease(&g_deviceLock, flags);
    return ok;
}

void DeviceIterate(DeviceType type, bool (*cb)(Device* dev, void* ctx), void* ctx)
{
    if (!cb) return;
    uint64_t flags = SpinLockAcquire(&g_deviceLock);
    for (uint32_t i = 0; i < g_deviceCount; ++i)
    {
        if (!DevicePtrLooksValid(g_devices[i])) continue;
        if (g_devices[i]->type == type)
        {
            if (!cb(g_devices[i], ctx))
            {
                SpinLockRelease(&g_deviceLock, flags);
                return;
            }
        }
    }
    SpinLockRelease(&g_deviceLock, flags);
}

} // namespace brook
