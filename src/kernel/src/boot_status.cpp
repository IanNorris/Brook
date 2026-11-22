#include "boot_status.h"
#include "apic.h"
#include "kprintf.h"
#include "net.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "vfs.h"
#if defined(BROOK_BRIDGE_V3)
#include "debug_bridge_v3.h"
#endif

namespace brook {
namespace {
constexpr uint32_t UsbTextCapacity = 6144;
char g_usbText[UsbTextCapacity];
uint32_t g_usbLength = 0;
volatile uint64_t g_usbGeneration = 0;
IrqSpinLock g_usbLock;
uint32_t (*volatile g_usbProvider)(char*, uint32_t) = nullptr;
SpinLock g_printLock;
bool g_enabled = false;
bool g_threadsStarted = false;

struct Worker { const char* name; void (*fn)(void*); void* arg; };
Worker g_workers[4];
uint32_t g_workerCount = 0;
SpinLock g_workerLock;

void PrintIp(const char* label, uint32_t ip)
{
    KPrintf("%s%u.%u.%u.%u", label, ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24);
}

uint32_t PrefixLength(uint32_t mask)
{
    uint32_t bits = 0;
    for (uint32_t m = mask; m; m &= m - 1) ++bits;
    return bits;
}

const char* DhcpName(uint8_t state)
{
    switch (state) {
    case 1: return "discovering";
    case 2: return "requesting";
    case 3: return "bound";
    default: return "idle";
    }
}

void StartWorker(const Worker& w)
{
    Process* thread = KernelThreadCreate(w.name, w.fn, w.arg);
    if (thread) SchedulerAddProcess(thread);
    else KPrintf("boot-status: worker %s could not start\n", w.name);
}

uint64_t NetSignature()
{
    uint64_t signature = NetIfCount();
    for (uint32_t i = 0; i < NetIfCount(); ++i) {
        const NetIf* nif = NetIfAt(i);
        uint8_t state = 0;
        DhcpIfState(i, &state, nullptr);
        signature = signature * 1000003u + (nif ? nif->ipAddr : 0) + state;
    }
    return signature;
}

void StatusThread(void*)
{
    // Boot output (desktop, launcher, compositor) settles within seconds;
    // print then, again at 30 s, and after any USB or network change.
    const uint64_t started = ApicTickCount();
    uint64_t printedUsb = ~0ull, printedNet = ~0ull, changedAt = 0;
    bool firstDone = false, secondDone = false;
    for (;;) {
        if (DhcpService()) changedAt = ApicTickCount();
        const uint64_t now = ApicTickCount();
        const uint64_t usb = __atomic_load_n(&g_usbGeneration, __ATOMIC_ACQUIRE);
        const uint64_t net = NetSignature();
        if ((usb != printedUsb || net != printedNet) && !changedAt) changedAt = now;
        const char* reason = nullptr;
        if (!firstDone && now - started >= 10000) { reason = "boot settled"; firstDone = true; }
        else if (!secondDone && now - started >= 30000) { reason = "30 s"; secondDone = true; }
        else if (firstDone && changedAt && now - changedAt >= 1500 &&
                 (usb != printedUsb || net != printedNet)) reason = "state changed";
        if (reason) {
            BootStatusPrint(reason);
            printedUsb = usb; printedNet = net; changedAt = 0;
        }
        SchedulerSleepMs(250);
    }
}
} // namespace

void BootStatusConfigure()
{
    if (auto* marker = VfsOpen("/etc/boot-status")) {
        VfsClose(marker);
        g_enabled = true;
        SerialPuts("boot-status: enabled (status block, persistent DHCP)\n");
    }
}

bool BootStatusEnabled() { return g_enabled; }

void BootStatusStartThreads()
{
    SpinLockAcquire(&g_workerLock);
    g_threadsStarted = true;
    const uint32_t count = g_workerCount;
    SpinLockRelease(&g_workerLock);
    for (uint32_t i = 0; i < count; ++i) StartWorker(g_workers[i]);
    if (!g_enabled) return;
    Process* thread = KernelThreadCreate("boot_status", StatusThread, nullptr);
    if (thread) SchedulerAddProcess(thread);
}
} // namespace brook

using namespace brook;

extern "C" void UsbStatusPublish(const char* text, uint32_t length)
{
    if (!text) return;
    if (length >= UsbTextCapacity) length = UsbTextCapacity - 1;
    const auto saved = IrqSpinLockAcquire(&g_usbLock);
    const bool same = length == g_usbLength && !memcmp(g_usbText, text, length);
    if (!same) {
        memcpy(g_usbText, text, length);
        g_usbText[length] = 0;
        g_usbLength = length;
        __atomic_fetch_add(&g_usbGeneration, 1, __ATOMIC_RELEASE);
    }
    IrqSpinLockRelease(&g_usbLock, saved);
}

extern "C" void UsbStatusSetProvider(uint32_t (*provider)(char* out, uint32_t capacity))
{
    g_usbProvider = provider;
    __atomic_fetch_add(&g_usbGeneration, 1, __ATOMIC_RELEASE);
}

extern "C" void UsbStatusChanged()
{
    __atomic_fetch_add(&g_usbGeneration, 1, __ATOMIC_RELEASE);
}

extern "C" void BootStatusPrint(const char* reason)
{
    if (!g_enabled) return;
    static char copy[UsbTextCapacity];
    SpinLockAcquire(&g_printLock);
    if (auto* provider = g_usbProvider) {
        const uint32_t length = provider(copy, UsbTextCapacity);
        copy[length < UsbTextCapacity ? length : UsbTextCapacity - 1] = 0;
    } else {
        const auto saved = IrqSpinLockAcquire(&g_usbLock);
        memcpy(copy, g_usbText, g_usbLength + 1);
        IrqSpinLockRelease(&g_usbLock, saved);
    }
    const uint64_t ms = ApicTickCount();
    KPrintf("==== BROOK STATUS (%s) t=%lu.%lus ====\n", reason ? reason : "-",
            static_cast<unsigned long>(ms / 1000), static_cast<unsigned long>((ms / 100) % 10));
    if (copy[0]) {
        // KPrintf lines one at a time so each reaches the console intact.
        char* line = copy;
        while (*line) {
            char* end = line;
            while (*end && *end != '\n') ++end;
            const bool more = *end == '\n';
            *end = 0;
            KPrintf("%s\n", line);
            line = more ? end + 1 : end;
        }
    } else KPrintf("USB no xHCI status published (driver absent or failed to load)\n");
    const uint32_t count = NetIfCount();
    if (!count) KPrintf("NET no network interface yet (DHCP starts when a NIC appears)\n");
    uint32_t bridgeIp = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const NetIf* nif = NetIfAt(i);
        if (!nif) continue;
        uint8_t state = 0; uint32_t attempts = 0;
        DhcpIfState(i, &state, &attempts);
        KPrintf("NET if%u MAC %02x:%02x:%02x:%02x:%02x:%02x DHCP %s attempts=%u", i,
                nif->mac.b[0], nif->mac.b[1], nif->mac.b[2], nif->mac.b[3], nif->mac.b[4], nif->mac.b[5],
                nif->ipAddr ? "bound" : DhcpName(state), attempts);
        if (nif->ipAddr) {
            PrintIp(" IP ", nif->ipAddr);
            KPrintf("/%u", PrefixLength(nif->netmask));
            if (nif->gateway) PrintIp(" gw ", nif->gateway);
            if (!bridgeIp) bridgeIp = nif->ipAddr;
        }
        KPrintf("\n");
    }
    if (bridgeIp) {
        PrintIp("BRIDGE endpoint tcp ", bridgeIp);
        KPrintf(":1234 (v3 operator auth)\n");
    } else KPrintf("BRIDGE no IPv4 address yet; DHCP keeps retrying\n");
#if defined(BROOK_BRIDGE_V3)
    DebugBridgeV3LogBoot();
#endif
    KPrintf("==== END STATUS ====\n");
    SpinLockRelease(&g_printLock);
}

extern "C" bool KernelWorkerStart(const char* name, void (*fn)(void*), void* arg)
{
    if (!fn) return false;
    SpinLockAcquire(&g_workerLock);
    if (g_threadsStarted) {
        SpinLockRelease(&g_workerLock);
        StartWorker({name, fn, arg});
        return true;
    }
    if (g_workerCount >= sizeof(g_workers) / sizeof(g_workers[0])) {
        SpinLockRelease(&g_workerLock);
        return false;
    }
    g_workers[g_workerCount++] = {name, fn, arg};
    SpinLockRelease(&g_workerLock);
    return true;
}
