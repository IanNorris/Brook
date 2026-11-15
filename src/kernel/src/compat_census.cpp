// compat_census.cpp -- kernel side of the Linux-compatibility census.
//
// SyscallDispatchInternal calls CompatCensusObserve only for results in the
// errno range; everything here runs on failing syscalls in process context
// (interrupts enabled, may touch user memory). See compat_census.h.

#include "compat_census_runtime.h"
#include "compat_census.h"
#include "apic.h"
#include "bridge_reply.h"
#include "net.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_strace.h"
#include "debug_overlay.h"

namespace brook {

namespace {

constexpr uint64_t kEnoent = 2, kEnodev = 19, kEinval = 22, kEnotty = 25, kEnosys = 38,
                   kEnoprotoopt = 92, kEprotonosupport = 93, kEsocktnosupport = 94,
                   kEopnotsupp = 95, kEafnosupport = 97;

CompatCensusTable<512> g_compatCensus;
CompatCensusTable<128> g_compatProcesses;

// Debug commands are serial per listener; a busy flag protects the single
// static snapshot buffer if two listeners ask at once.
CompatEntry g_compatSnapshot[512];
uint32_t g_compatSnapshotBusy;

bool Unsupported(uint64_t error)
{
    return error == kEnosys || error == kEinval || error == kEopnotsupp;
}

uint64_t FdTypeOf(uint64_t fd)
{
    Process* proc = ProcessCurrent();
    if (!proc || fd >= MAX_FDS) return 0xff;
    FdRef ref(proc, static_cast<int>(fd));
    return ref ? static_cast<uint64_t>(ref->type) : 0xff;
}

// Path syscalls whose ENOENT under /proc, /sys or /dev is recorded.
// Returns the argument index of the path, or -1.
int PathArgument(uint64_t num)
{
    switch (num) {
    case 2:   // open
    case 4:   // stat
    case 6:   // lstat
    case 21:  // access
    case 80:  // chdir
    case 89:  // readlink
    case 137: // statfs
        return 0;
    case 257: // openat
    case 262: // newfstatat
    case 267: // readlinkat
    case 269: // faccessat
    case 332: // statx
    case 437: // openat2
    case 439: // faccessat2
        return 1;
    default:
        return -1;
    }
}

} // namespace

void CompatCensusObserve(uint64_t num, int64_t result, const uint64_t* args, uint64_t origin)
{
    const uint64_t error = static_cast<uint64_t>(-result);
    CompatKey key{};
    key.nr = static_cast<uint32_t>(num);
    key.error = static_cast<uint16_t>(error);
    char text[CompatTextBytes] = {};
    bool record = false;

    switch (num) {
    case 16: // ioctl
        if (error == kEnotty || Unsupported(error)) {
            key.kind = CompatKind::Ioctl;
            key.a = FdTypeOf(args[0]);
            key.b = args[1] & 0xffffffffull;
            record = true;
        }
        break;
    case 72: // fcntl
        if (Unsupported(error)) { key.kind = CompatKind::Fcntl; key.b = args[1]; record = true; }
        break;
    case 157: // prctl
        if (Unsupported(error)) { key.kind = CompatKind::Prctl; key.b = args[0]; record = true; }
        break;
    case 158: // arch_prctl
        if (Unsupported(error)) { key.kind = CompatKind::ArchPrctl; key.b = args[0]; record = true; }
        break;
    case 54: case 55: // setsockopt, getsockopt
        if (error == kEnoprotoopt || Unsupported(error)) {
            key.kind = CompatKind::Sockopt;
            key.a = args[1] & 0xffffffffull;
            key.b = args[2] & 0xffffffffull;
            key.c = FdTypeOf(args[0]);
            record = true;
        }
        break;
    case 41: case 53: // socket, socketpair
        if (error == kEafnosupport || error == kEprotonosupport || error == kEsocktnosupport ||
            Unsupported(error)) {
            key.kind = CompatKind::Socket;
            key.a = args[0] & 0xffffffffull;
            key.b = args[1] & 0xffffffffull & ~0x80800ull; // drop SOCK_NONBLOCK|SOCK_CLOEXEC
            key.c = args[2] & 0xffffffffull;
            record = true;
        }
        break;
    case 56: // clone
        if (Unsupported(error)) { key.kind = CompatKind::Clone; key.b = args[0]; record = true; }
        break;
    case 435: // clone3: flags are the first field of struct clone_args
        if (Unsupported(error)) {
            uint64_t flags = 0;
            if (args[1] >= 8) (void)CopyFromUser(&flags, args[0], sizeof(flags));
            key.kind = CompatKind::Clone;
            key.b = flags;
            record = true;
        }
        break;
    case 9: // mmap
        if (error == kEnodev || Unsupported(error)) {
            key.kind = CompatKind::Mmap;
            key.a = args[2];
            key.b = args[3];
            key.c = (args[3] & 0x20) ? 0 : FdTypeOf(args[4]); // MAP_ANONYMOUS
            record = true;
        }
        break;
    case 10: // mprotect
        if (Unsupported(error)) { key.kind = CompatKind::Mmap; key.a = args[2]; record = true; }
        break;
    case 25: // mremap
        if (Unsupported(error)) { key.kind = CompatKind::Mmap; key.b = args[3]; record = true; }
        break;
    case 28: // madvise
        if (Unsupported(error)) { key.kind = CompatKind::Mmap; key.b = args[2]; record = true; }
        break;
    default:
        if (error == kEnoent) {
            const int index = PathArgument(num);
            if (index < 0) break;
            char raw[96];
            const int copied = CopyUserCStringResult(args[index], raw, sizeof(raw));
            if ((copied == 0 || copied == -36) && CompatNormalizePath(raw, text, sizeof(text))) {
                key.kind = CompatKind::Path;
                key.textHash = CompatTextHash(text);
                record = true;
            }
        }
        break;
    }
    if (!record && error == kEnosys) {
        key.kind = CompatKind::Syscall;
        key.a = origin;
        record = true;
    }
    if (!record) return;

    Process* proc = ProcessCurrent();
    const uint32_t pid = proc ? proc->pid : 0;
    const char* name = proc ? proc->name : "?";
    const uint64_t tick = ApicTickCount();
    const CompatNote note = g_compatCensus.Note(key, text[0] ? text : nullptr, tick, pid, name);

    CompatKey owner{};
    owner.kind = CompatKind::ProcessName;
    owner.error = static_cast<uint16_t>(error == kEnosys ? kEnosys : 0);
    char ownerName[CompatNameBytes];
    CompatCopyText(ownerName, sizeof(ownerName), name);
    owner.textHash = CompatTextHash(ownerName);
    (void)g_compatProcesses.Note(owner, ownerName, tick, pid, name);

    // One line per distinct key (bounded by the table size), on serial and
    // in the kernel log ring so hw_logs and the last-log record see it.
    // The quiet ENOSYS set stays quiet but is still counted.
    if (note == CompatNote::New && !(key.kind == CompatKind::Syscall && origin == CompatOriginQuiet)) {
        char line[200];
        CompatText t(line, sizeof(line));
        t.str("COMPAT: new ");
        CompatDescribe(t, key, text, 0, nullptr);
        t.str(" pid "); t.num(pid); t.str(" ("); t.str(name); t.str(")\n");
        SerialPuts(line);
        DebugOverlayPuts(line);
    }
}

static void CompatReplyAppend(const char* text);

static void CompatEntryJson(BridgeReplyBuffer<CompatReplyAppend>& r, const CompatEntry& e)
{
    r.text("{\"kind\":");r.quoted(CompatKindName(e.key.kind));
    if (e.key.kind != CompatKind::ProcessName) {
        r.text(",");r.field("nr",e.key.nr);
        if (const char* name = SyscallNameOrNull(e.key.nr)) {r.text(",\"syscall\":");r.quoted(name);}
    }
    r.text(",");r.field("errno",e.key.error);
    r.text(",\"a\":\"0x");r.number(e.key.a,true);
    r.text("\",\"b\":\"0x");r.number(e.key.b,true);
    r.text("\",\"c\":\"0x");r.number(e.key.c,true);r.text("\"");
    if (e.text[0]) {r.text(",\"text\":");r.quoted(e.text);}
    r.text(",");r.field("count",e.count);
    r.text(",");r.field("first_ms",e.firstTick);
    r.text(",");r.field("last_ms",e.lastTick);
    r.text(",");r.field("first_pid",e.firstPid);
    r.text(",\"first_name\":");r.quoted(e.firstName);
    r.text("}");
}

// The reply is assembled in one static buffer and sent with a single
// DebugChannelSend: many small sends are slow on the guest TCP path.
static constexpr uint32_t kReplyBytes = 256 * 1024;
static char g_compatReply[kReplyBytes];
static uint32_t g_compatReplyUsed;
static bool g_compatReplyTruncated;

static void CompatReplyAppend(const char* text)
{
    while (*text) {
        if (g_compatReplyUsed + 1 >= kReplyBytes) {g_compatReplyTruncated = true;return;}
        g_compatReply[g_compatReplyUsed++] = *text++;
    }
}

void CompatCensusReplyJson(uint32_t limit)
{
    using Reply = BridgeReplyBuffer<CompatReplyAppend>;
    if (__atomic_exchange_n(&g_compatSnapshotBusy, 1u, __ATOMIC_ACQUIRE)) {
        DebugChannelSend("{\"error\":\"compat census busy\"}\n");
        return;
    }
    if (!limit) limit = 128;
    if (limit > 512) limit = 512;
    g_compatReplyUsed = 0;
    g_compatReplyTruncated = false;
    Reply r;
    r.text("{");r.field("now_ms",ApicTickCount());
    r.text(",");r.field("total",__atomic_load_n(&g_compatCensus.total,__ATOMIC_RELAXED));
    r.text(",");r.field("overflow",__atomic_load_n(&g_compatCensus.overflow,__ATOMIC_RELAXED));
    r.text(",");r.field("resets",__atomic_load_n(&g_compatCensus.resets,__ATOMIC_RELAXED));
    uint32_t used = 0;
    uint32_t n = g_compatCensus.Snapshot(g_compatSnapshot, limit, &used);
    r.text(",");r.field("capacity",512);r.text(",");r.field("used",used);
    r.text(",\"entries\":[");
    for (uint32_t i = 0; i < n; ++i) {if (i) r.text(",");CompatEntryJson(r,g_compatSnapshot[i]);}
    uint32_t processUsed = 0;
    n = g_compatProcesses.Snapshot(g_compatSnapshot, 128, &processUsed);
    r.text("],");r.field("process_overflow",__atomic_load_n(&g_compatProcesses.overflow,__ATOMIC_RELAXED));
    r.text(",\"processes\":[");
    for (uint32_t i = 0; i < n; ++i) {if (i) r.text(",");CompatEntryJson(r,g_compatSnapshot[i]);}
    r.text("],\"scope\":\"failing syscalls since boot or reset, sorted by count. kind=syscall: ENOSYS by nr, a=origin (0 handler, 1 unimplemented, 2 quiet, 3 out of range). ioctl: a=FdType, b=request. fcntl/prctl/arch_prctl: b=cmd/option. sockopt: a=level, b=optname, c=FdType. socket: a=family, b=type, c=protocol. clone: b=flags. mmap: nr=mmap/mprotect/mremap/madvise, a=prot, b=flags/advice, c=FdType (0 anonymous). path: ENOENT under /proc,/sys,/dev with numeric components as N. processes: per process name; errno 38 = ENOSYS hits, 0 = other unsupported hits. ticks are ms since boot\"}\n");
    r.send();
    g_compatReply[g_compatReplyUsed] = 0;
    if (g_compatReplyTruncated)
        DebugChannelSend("{\"error\":\"compat census reply exceeds 256 KiB; request fewer entries\"}\n");
    else
        DebugChannelSend(g_compatReply);
    __atomic_store_n(&g_compatSnapshotBusy, 0u, __ATOMIC_RELEASE);
}

uint64_t CompatCensusTotal()
{
    return __atomic_load_n(&g_compatCensus.total, __ATOMIC_RELAXED);
}

void CompatCensusReset()
{
    g_compatCensus.Reset();
    g_compatProcesses.Reset();
}

uint32_t CompatCensusFormatSummary(char* out, uint32_t capacity, uint32_t limit)
{
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (__atomic_exchange_n(&g_compatSnapshotBusy, 1u, __ATOMIC_ACQUIRE)) return 0;
    if (limit > 512) limit = 512;
    const uint32_t n = g_compatCensus.Snapshot(g_compatSnapshot, limit);
    CompatText t(out, capacity);
    for (uint32_t i = 0; i < n; ++i) {
        const CompatEntry& e = g_compatSnapshot[i];
        CompatDescribe(t, e.key, e.text, e.count, e.firstName);
        t.str("\n");
    }
    const uint32_t used = t.used;
    out[used] = 0;
    __atomic_store_n(&g_compatSnapshotBusy, 0u, __ATOMIC_RELEASE);
    return used;
}

} // namespace brook
