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
#include "memory/mapping_ranges.h"

namespace brook {
namespace {
nvdrm::DeviceInfo g_dev;
const nvdrm::Backend* g_be = nullptr;
volatile bool g_published = false;
bool g_marker = false;   // /etc/nv-drm, read once at publish
bool MarkerExists(const char* path) { auto* f = VfsOpen(path, VFS_O_STREAM); if (!f) return false; VfsRelease(f); return true; }
char g_attrUevent[96], g_attrDevice[16], g_attrSubVendor[16], g_attrSubDevice[16], g_attrRevision[16];
struct Attr { const char* path; const char* content; };
Attr g_attrs[7];
}  // namespace

struct NvDrmFile { uint32_t references; KMutex lock; nvdrm::File core; };

// ---- completion poller: queued binds/execs and in-flight fences advance without an ioctl.
namespace {
constexpr uint32_t kMaxFiles = 32;
NvDrmFile* g_files[kMaxFiles];
KMutex g_filesLock;
bool g_pollerStarted = false;
bool Busy(const nvdrm::File& f) {
    if (f.job_count) return true;
    for (const auto& c : f.channels) if (c.used && (c.job_count || c.in_count)) return true;
    return false;
}
void Poller(void*) {
    for (;;) {
        NvDrmFile* list[kMaxFiles]; uint32_t n = 0;
        KMutexLock(&g_filesLock);
        for (auto* f : g_files) if (f) { NvDrmFileRef(f); list[n++] = f; }
        KMutexUnlock(&g_filesLock);
        bool busy = false;
        for (uint32_t i = 0; i < n; ++i) {
            KMutexLock(&list[i]->lock);
            if (list[i]->core.be) { nvdrm::Pump(list[i]->core); nvdrm::PumpExec(list[i]->core); busy |= Busy(list[i]->core); }
            KMutexUnlock(&list[i]->lock);
            NvDrmFileUnref(list[i]);
        }
        SchedulerSleepMs(busy ? 1 : 20);
    }
}
void Register(NvDrmFile* f, bool add) {
    KMutexLock(&g_filesLock);
    for (auto*& slot : g_files) if (add ? !slot : slot == f) { slot = add ? f : nullptr; break; }
    const bool start = add && !g_pollerStarted;
    g_pollerStarted = true;
    KMutexUnlock(&g_filesLock);
    if (start) { Process* t = KernelThreadCreate("nv_drm_poll", Poller, nullptr, 1); if (t) SchedulerAddProcess(t); }
}
}  // namespace

static void Hex(char* out, uint32_t v) {
    static const char d[] = "0123456789abcdef";
    char t[12]; int n = 0;
    do { t[n++] = d[v & 15]; v >>= 4; } while (v);
    int o = 0; out[o++] = '0'; out[o++] = 'x';
    if (n < 4) for (int i = n; i < 4; ++i) out[o++] = '0';
    while (n) out[o++] = t[--n];
    out[o++] = '\n'; out[o] = 0;
}

void NvDrmPublish(const nvdrm::DeviceInfo& info, const nvdrm::Backend* backend) {
    g_dev = info; g_dev.valid = true; g_be = backend;
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
    f->core.Init(&g_dev, g_be);
    static bool once = false; if (!once) { once = true; KMutexInit(&g_filesLock); }
    Register(f, true);
    return f;
}
void NvDrmFileRef(NvDrmFile* f) { if (f) __atomic_fetch_add(&f->references, 1, __ATOMIC_RELAXED); }
void NvDrmFileUnref(NvDrmFile* f) {
    if (f && __atomic_fetch_sub(&f->references, 1, __ATOMIC_ACQ_REL) == 1) {
        Register(f, false);
        KMutexLock(&f->lock); f->core.Close(); KMutexUnlock(&f->lock);
        kfree(f);
    }
}
bool NvDrmMapGart(NvDrmFile* f, uint64_t offset, uint64_t length, uint64_t* phys, MappingBacking** life) {
    if (!f || !length) return false;
    KMutexLock(&f->lock);
    const uint64_t token = offset & ~0xffffffffull, rel = offset & 0xffffffffull;
    const nvdrm::Bo* b = nvdrm::FindBoByToken(f->core, token);
    bool ok = b && b->aper() == nvdrm::Aper::Sys && rel + length <= b->size && rel + length > rel;
    if (ok) {
        const auto* pages = reinterpret_cast<const NvDrmSysPages*>(b->backing);
        *phys = pages->phys + rel; *life = pages->life; MappingBackingRef(pages->life);
    }
    KMutexUnlock(&f->lock);
    return ok;
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
    int64_t r;
    if (nr == nvdrm::NR_SYNCOBJ_WAIT || nr == nvdrm::NR_SYNCOBJ_TIMELINE_WAIT) {
        // waits sleep without the file lock (GpuSyncTable has its own): retire finished work
        // under the lock first, then wait without touching queues or rings (the poller pumps)
        KMutexLock(&f->lock);
        if (f->core.be) { if (f->core.job_count) nvdrm::Pump(f->core); nvdrm::PumpExec(f->core); }
        KMutexUnlock(&f->lock);
        r = nvdrm::Ioctl(f->core, u, cmd, arg, false);
    }
    else { KMutexLock(&f->lock); r = nvdrm::Ioctl(f->core, u, cmd, arg); KMutexUnlock(&f->lock); }
    // M5 bring-up: failing ioctls (except expected waits/busy) go to the kernel log, rate-limited.
    static uint32_t logged = 0;
    if (r < 0 && r != nvdrm::kETIME && r != nvdrm::kEBUSY && logged < 400) {
        ++logged;
        KPrintf("nv-drm: ioctl nr 0x%x size %u -> %ld (%s)\n", nr, nvdrm::IocSize(cmd), (long)r, f->core.why);
        f->core.why = "";
    }
    return r;
}
}  // namespace brook
