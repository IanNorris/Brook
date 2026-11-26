#include "nv_drm.h"
#include "memory/heap.h"
#include "sync/kmutex.h"
#include "string.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "apic.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_time.h"
#include "kprintf.h"

namespace brook {
namespace {
nvdrm::DeviceInfo g_dev;
volatile bool g_published = false;
bool g_marker = false;   // /etc/nv-drm, read once at publish
bool MarkerExists(const char* path) { auto* f = VfsOpen(path, VFS_O_STREAM); if (!f) return false; VfsRelease(f); return true; }
char g_attrUevent[96], g_attrDevice[16], g_attrSubVendor[16], g_attrSubDevice[16], g_attrRevision[16];
struct Attr { const char* path; const char* content; };
Attr g_attrs[7];
}  // namespace

struct NvDrmFile { uint32_t references; KMutex lock; nvdrm::File core; };

static void Hex(char* out, uint32_t v) {
    static const char d[] = "0123456789abcdef";
    char t[12]; int n = 0;
    do { t[n++] = d[v & 15]; v >>= 4; } while (v);
    int o = 0; out[o++] = '0'; out[o++] = 'x';
    if (n < 4) for (int i = n; i < 4; ++i) out[o++] = '0';
    while (n) out[o++] = t[--n];
    out[o++] = '\n'; out[o] = 0;
}

void NvDrmPublish(const nvdrm::DeviceInfo& info) {
    g_dev = info; g_dev.valid = true;
    // libdrm drmGetDevices2: literal attribute files, PCI slot of the real GPU (02:00.0).
    const char* ue = "DRIVER=nouveau\nPCI_SLOT_NAME=0000:02:00.0\n";
    uint32_t i = 0; for (; ue[i] && i < sizeof(g_attrUevent) - 1; ++i) g_attrUevent[i] = ue[i]; g_attrUevent[i] = 0;
    Hex(g_attrDevice, info.pci_device); Hex(g_attrSubVendor, 0x19da); Hex(g_attrSubDevice, 0x3500);
    { static const char d[] = "0123456789abcdef"; const char r[] = {'0', 'x', d[(info.revision >> 4) & 15], d[info.revision & 15], '\n', 0};
      memcpy(g_attrRevision, r, sizeof(r)); }   // libdrm parses %x; real sysfs prints two digits
    g_attrs[0] = {"/sys/dev/char/226:130/device/uevent", g_attrUevent};
    g_attrs[1] = {"/sys/dev/char/226:130/device/vendor", "0x10de\n"};
    g_attrs[2] = {"/sys/dev/char/226:130/device/device", g_attrDevice};
    g_attrs[3] = {"/sys/dev/char/226:130/device/subsystem_vendor", g_attrSubVendor};
    g_attrs[4] = {"/sys/dev/char/226:130/device/subsystem_device", g_attrSubDevice};
    g_attrs[5] = {"/sys/dev/char/226:130/device/revision", g_attrRevision};
    g_attrs[6] = {nullptr, nullptr};
    g_marker = MarkerExists("/etc/nv-drm");
    __atomic_store_n(&g_published, true, __ATOMIC_RELEASE);
    KPrintf("nv-drm: renderD130 published (chipset %x, device %x, VRAM %lu MiB, %u GPC / %u TPC)%s\n", info.chipset,
            info.pci_device, (unsigned long)(info.vram_bytes >> 20), info.gpc_count, info.tpc_count,
            g_marker ? "" : " - node off (no /etc/nv-drm)");
}
void NvDrmRetract() { __atomic_store_n(&g_published, false, __ATOMIC_RELEASE); }
bool NvDrmNodeEnabled() { return __atomic_load_n(&g_published, __ATOMIC_ACQUIRE) && g_marker; }
const char* NvDrmSysAttrContent(const char* path) {
    if (!NvDrmNodeEnabled()) return nullptr;
    for (const Attr* a = g_attrs; a->path; ++a) if (!strcmp(path, a->path)) return a->content;
    return nullptr;
}
bool NvDrmSysIsDir(const char* path) {
    return NvDrmNodeEnabled() && (!strcmp(path, "/sys/dev/char/226:130/device") || !strcmp(path, "/sys/dev/char/226:130/device/drm"));
}

NvDrmFile* NvDrmFileCreate() {
    if (!NvDrmNodeEnabled()) return nullptr;
    auto* f = static_cast<NvDrmFile*>(kmalloc(sizeof(NvDrmFile)));
    if (!f) return nullptr;
    memset(f, 0, sizeof(*f));
    f->references = 1; KMutexInit(&f->lock);
    f->core.Init(&g_dev);
    return f;
}
void NvDrmFileRef(NvDrmFile* f) { if (f) __atomic_fetch_add(&f->references, 1, __ATOMIC_RELAXED); }
void NvDrmFileUnref(NvDrmFile* f) {
    if (f && __atomic_fetch_sub(&f->references, 1, __ATOMIC_ACQ_REL) == 1) { f->core.Close(); kfree(f); }
}
int64_t NvDrmIoctl(NvDrmFile* f, uint64_t cmd, uint64_t arg) {
    if (!f) return nvdrm::kENOTTY;
    const nvdrm::UserAccess u{nullptr,
        [](void*, void* dst, uint64_t src, uint64_t n) { return CopyFromUser(dst, src, n); },
        [](void*, uint64_t dst, const void* src, uint64_t n) { return CopyToUser(dst, src, n); },
        [](void*) { return ClockNowNs(1); },
        [](void*) { Process* self = ProcessCurrent(); if (!self || HasPendingSignals()) return false;
                    self->wakeupTick = ApicTickCount() + 1; SchedulerBlockInterruptible(self); return !HasPendingSignals(); }};
    // Syncobj waits sleep; everything else is short. One lock per file keeps the tables consistent.
    const uint32_t nr = nvdrm::IocNr(cmd);
    if (nr == nvdrm::NR_SYNCOBJ_WAIT) return nvdrm::Ioctl(f->core, u, cmd, arg);   // GpuSyncTable has its own lock
    KMutexLock(&f->lock);
    const int64_t r = nvdrm::Ioctl(f->core, u, cmd, arg);
    KMutexUnlock(&f->lock);
    return r;
}
}  // namespace brook
