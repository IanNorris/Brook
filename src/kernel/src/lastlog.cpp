// lastlog.cpp -- keep the kernel log tail and compat census on disk so the
// previous boot's last seconds are readable after a hang and power cycle.
//
// A low-priority kernel thread writes one fixed-size record (lastlog_format.h)
// at most every LastLogIntervalMs, only when serial output or the census has
// changed, to /nix/var/log/brook/lastlog{0,1}.bin on the persistent ext2
// store. It uses the ordinary VFS write + fsync path that applications use;
// nothing here runs in IRQ context, takes GPU/display locks or touches raw
// block devices. The serial-tail snapshot is a lock-free copy.
//
// Before its first write in a boot the thread reads both slots, keeps the
// newest valid record (the previous boot's last one) in RAM for the debug
// bridge, and copies it to previous.bin. It never writes once shutdown has
// begun, so it cannot race the final sync and clean-marker transaction.

#include "lastlog.h"
#include "lastlog_format.h"
#include "compat_census.h"
#include "compat_census_runtime.h"
#include "apic.h"
#include "bridge_reply.h"
#include "debug_epoch.h"
#include "debug_overlay.h"
#include "mount_namespace.h"
#include "net.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "shutdown.h"
#include "string.h"
#include "vfs.h"
#include "writable_desktop.h"

uint64_t SoftRandU64();
bool RdrandU64(uint64_t* out);

namespace brook {

namespace {

constexpr uint32_t LastLogIntervalMs = 5000;
const char* const kDirectories[] = {"/nix/var", "/nix/var/log", "/nix/var/log/brook"};
const char* const kSlots[2] = {"/nix/var/log/brook/lastlog0.bin", "/nix/var/log/brook/lastlog1.bin"};
const char* const kPreviousPath = "/nix/var/log/brook/previous.bin";

enum class State : uint32_t { Waiting, Ready, Failed, Stopped };
const char* StateName(State s)
{
    switch (s) {
    case State::Waiting: return "waiting_for_store";
    case State::Ready: return "ready";
    case State::Failed: return "failed";
    case State::Stopped: return "stopped_for_shutdown";
    }
    return "?";
}

uint8_t g_file[LastLogFileBytes];
char g_text[LastLogPayloadMax];
uint8_t g_previous[LastLogFileBytes];
LastLogRecord g_previousRecord;
bool g_previousPublished;          // release-published after g_previous is filled
char g_bootId[33];

bool g_enabled = true;
bool g_flushRequested;
bool g_spawned;
uint32_t g_state = static_cast<uint32_t>(State::Waiting);
State LoadState() { return static_cast<State>(__atomic_load_n(&g_state, __ATOMIC_ACQUIRE)); }
void StoreState(State s) { __atomic_store_n(&g_state, static_cast<uint32_t>(s), __ATOMIC_RELEASE); }
uint64_t g_sequence;               // next sequence to write
uint64_t g_writes, g_failures, g_lastWriteMs, g_lastBytes;
int g_lastError, g_prepareError;
uint64_t g_lastSerial = ~uint64_t(0);
uint64_t g_lastCensus = ~uint64_t(0);

// The bridge boot epoch once a bridge has established one (hw_attach);
// before that a random per-boot nonce, so records from different boots
// are still distinguishable.
void RefreshBootId()
{
    char epoch[33];
    DebugEpochCopy(epoch);
    if (strcmp(epoch, "00000000000000000000000000000000")) { memcpy(g_bootId, epoch, sizeof(epoch)); return; }
    if (g_bootId[0]) return;
    const char* hex = "0123456789abcdef";
    uint64_t parts[2];
    for (uint64_t& part : parts) if (!RdrandU64(&part)) part = SoftRandU64();
    for (uint32_t i = 0; i < 32; ++i) g_bootId[i] = hex[(parts[i / 16] >> (4 * (i % 16))) & 15];
    g_bootId[32] = 0;
}

bool PersistentStoreMounted()
{
#if defined(BROOK_SSD_AUTO_RECOVERY)
    // Guarded SSD boots: only the admitted writable desktop store, never a
    // volume the guard checker may still be inspecting or repairing.
    const auto store = WritableDesktopRead();
    if (!store.ready) return false;
#endif
    MountNamespace* ns = VfsMountNamespaceInitial();
    const uint32_t count = VfsMountNamespaceCount(ns);
    for (uint32_t i = 0; i < count; ++i) {
        const VfsMountSnapshot mount = VfsMountNamespaceSnapshotAt(ns, i);
        if (mount.used && !strcmp(mount.mountPoint, "/nix") &&
            (!strcmp(mount.fsType, "ext2") || !strcmp(mount.fsType, "ext4")))
            return true;
    }
    return false;
}

bool ShutdownBegun()
{
    return !ShutdownLaunchAllowed();
}

int ReadWhole(const char* path, uint8_t* out)
{
    VnodeStat st{};
    if (VfsStatPath(path, &st)) return -2;
    if (st.isDir || st.size != LastLogFileBytes) return -22;
    Vnode* node = VfsOpen(path, VFS_O_READ);
    if (!node) return -2;
    uint64_t offset = 0;
    const int n = VfsRead(node, out, LastLogFileBytes, &offset);
    VfsRelease(node);
    return n == static_cast<int>(LastLogFileBytes) ? 0 : -5;
}

// Overwrite a whole fixed-size file in place and fsync it. Creation (and
// extension of a wrong-sized file) happens only from Prepare.
int WriteWhole(const char* path, const uint8_t* data, bool create)
{
    Vnode* node = create ? VfsOpenMode(path, VFS_O_WRITE | VFS_O_READWRITE | VFS_O_CREATE, 0644)
                         : VfsOpen(path, VFS_O_WRITE | VFS_O_READWRITE);
    if (!node) return -2;
    uint64_t offset = 0;
    const int n = VfsWrite(node, data, LastLogFileBytes, &offset);
    const int synced = VfsSync(node);
    VfsRelease(node);
    if (n != static_cast<int>(LastLogFileBytes)) return n < 0 ? n : -5;
    return synced;
}

int Prepare()
{
    RefreshBootId();
    for (const char* dir : kDirectories) {
        const int r = VfsMkdir(dir, 0755);
        if (r && r != -17) return r;
    }
    LastLogRecord slots[2];
    for (uint32_t i = 0; i < 2; ++i) {
        if (ReadWhole(kSlots[i], g_file)) continue;
        slots[i] = LastLogDecode(g_file, LastLogFileBytes);
        const int newest = LastLogNewest(slots[0], slots[1]);
        if (slots[i].valid && newest == static_cast<int>(i)) {
            memcpy(g_previous, g_file, LastLogFileBytes);
            g_previousRecord = slots[i];
        }
    }
    if (LastLogNewest(slots[0], slots[1]) < 0) g_previousRecord = LastLogRecord{};
    g_sequence = LastLogNextSequence(slots[0], slots[1]);
    if (g_previousRecord.valid) __atomic_store_n(&g_previousPublished, true, __ATOMIC_RELEASE);

    // One-time creation at full size; later writes overwrite in place.
    memset(g_file, 0, LastLogFileBytes);
    for (uint32_t i = 0; i < 2; ++i) {
        VnodeStat st{};
        if (!VfsStatPath(kSlots[i], &st) && !st.isDir && st.size == LastLogFileBytes) continue;
        if (const int r = WriteWhole(kSlots[i], g_file, true)) return r;
    }
    if (g_previousRecord.valid) {
        VnodeStat st{};
        const bool exists = !VfsStatPath(kPreviousPath, &st) && !st.isDir && st.size == LastLogFileBytes;
        (void)WriteWhole(kPreviousPath, g_previous, !exists);
    }
    return 0;
}

int WriteRecord()
{
    CompatText t(g_text, sizeof(g_text));
    const uint64_t serial = SerialTailTotal();
    RefreshBootId();
    t.str("brook lastlog v1 boot="); t.str(g_bootId);
    t.str(" seq="); t.num(g_sequence);
    t.str(" uptime_ms="); t.num(ApicTickCount());
    t.str(" serial_bytes="); t.num(serial);
    t.str(" census_total="); t.num(CompatCensusTotal());
    t.str("\n== compat census (top 40 by count) ==\n");
    if (t.used + 8192 < sizeof(g_text)) {
        t.used += CompatCensusFormatSummary(g_text + t.used, 8192, 40);
        g_text[t.used] = 0;
    }
    // The serial tail is a superset of the kernel log ring (KPrintf and
    // KLog also print to serial) and includes SerialPrintf diagnostics.
    t.str("== serial tail (newest bytes that fit) ==\n");
    uint32_t bytes = t.used + SerialTailSnapshot(g_text + t.used, sizeof(g_text) - t.used);
    if (bytes > LastLogPayloadMax) bytes = LastLogPayloadMax;

    LastLogEncode(g_file, g_sequence, g_bootId, reinterpret_cast<const uint8_t*>(g_text), bytes);
    const int r = WriteWhole(kSlots[LastLogSlotFor(g_sequence)], g_file, false);
    if (r) {
        // Retry the same sequence and slot: the other slot keeps the last
        // good record until a write succeeds.
        ++g_failures;
        g_lastError = r;
        return r;
    }
    ++g_writes;
    g_lastBytes = bytes;
    g_lastWriteMs = ApicTickCount();
    g_lastSerial = serial;
    if (g_sequence != ~uint64_t(0)) ++g_sequence;
    return 0;
}

void LastLogThread(void*)
{
    uint32_t waitedMs = LastLogIntervalMs;
    for (;;) {
        SchedulerSleepMs(250);
        waitedMs += 250;
        if (ShutdownBegun()) {
            StoreState(State::Stopped);
            continue;
        }
        const bool flush = __atomic_exchange_n(&g_flushRequested, false, __ATOMIC_ACQ_REL);
        if (!flush && (waitedMs < LastLogIntervalMs || !__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE)))
            continue;
        waitedMs = 0;
        State state = LoadState();
        if (state == State::Failed || state == State::Stopped) continue;
        if (state == State::Waiting) {
            if (!PersistentStoreMounted()) continue;
            g_prepareError = Prepare();
            state = g_prepareError ? State::Failed : State::Ready;
            StoreState(state);
            if (g_prepareError) {
                char line[96];
                CompatText t(line, sizeof(line));
                t.str("lastlog: store preparation failed error=-"); t.num(static_cast<uint64_t>(-g_prepareError)); t.str("\n");
                SerialPuts(line);
                continue;
            }
            char line[160];
            CompatText t(line, sizeof(line));
            t.str("lastlog: ready (/nix/var/log/brook) next_seq="); t.num(g_sequence);
            t.str(g_previousRecord.valid ? " previous=" : " previous=none");
            if (g_previousRecord.valid) { t.str(g_previousRecord.bootId); t.str(" seq="); t.num(g_previousRecord.sequence); }
            t.str("\n");
            SerialPuts(line);
            DebugOverlayPuts(line);
        }
        if (!flush && SerialTailTotal() == g_lastSerial && CompatCensusTotal() == g_lastCensus) continue;
        g_lastCensus = CompatCensusTotal();
        (void)WriteRecord();
    }
}

using Reply = BridgeReplyBuffer<DebugChannelSend>;

void RecordJson(Reply& r, const LastLogRecord& rec)
{
    r.text("{\"valid\":"); r.text(rec.valid ? "true" : "false");
    if (rec.valid) {
        r.text(","); r.field("sequence", rec.sequence);
        r.text(","); r.field("bytes", rec.payloadBytes);
        r.text(","); r.field("crc32", rec.payloadCrc);
        r.text(",\"boot_id\":"); r.quoted(rec.bootId);
    }
    r.text("}");
}

bool Decimal(const char*& p, uint32_t& out)
{
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + static_cast<uint64_t>(*p++ - '0'); if (v > 0xffffffffu) return false; }
    out = static_cast<uint32_t>(v);
    return true;
}

} // namespace

void LastLogStart()
{
    if (__atomic_exchange_n(&g_spawned, true, __ATOMIC_ACQ_REL)) return;
    Process* thread = KernelThreadCreate("lastlog", LastLogThread, nullptr, 3);
    if (!thread) { SerialPuts("lastlog: thread creation failed\n"); return; }
    SchedulerAddProcess(thread);
}

bool LastLogHandleCommand(const char* cmd)
{
    auto prefix = [cmd](const char* p) { uint32_t i = 0; for (; p[i]; ++i) if (cmd[i] != p[i]) return false; return true; };
    if (!prefix("lastlog")) return false;
    const char* rest = cmd + 7;
    if (!strcmp(rest, " on") || !strcmp(rest, " off")) {
        __atomic_store_n(&g_enabled, rest[2] == 'n', __ATOMIC_RELEASE);
    } else if (!strcmp(rest, " flush")) {
        __atomic_store_n(&g_flushRequested, true, __ATOMIC_RELEASE);
    } else if (prefix("lastlog previous ")) {
        const char* p = cmd + 17;
        uint32_t offset = 0, count = 0;
        if (!Decimal(p, offset) || *p++ != ' ' || !Decimal(p, count) || *p || count > 4096) {
            DebugChannelSend("{\"lastlog_error\":\"Expected: lastlog previous OFFSET COUNT (COUNT 0..4096)\"}\n");
            return true;
        }
        if (!__atomic_load_n(&g_previousPublished, __ATOMIC_ACQUIRE)) {
            DebugChannelSend("{\"lastlog_error\":\"No previous-boot record (store not ready yet, or none on disk)\"}\n");
            return true;
        }
        const uint32_t bytes = g_previousRecord.payloadBytes;
        if (offset > bytes) offset = bytes;
        if (count > bytes - offset) count = bytes - offset;
        Reply r;
        r.text("{\"lastlog_api\":1,\"record\":"); RecordJson(r, g_previousRecord);
        r.text(","); r.field("offset", offset); r.text(","); r.field("count", count);
        r.text(",\"more\":"); r.text(offset + count < bytes ? "true" : "false");
        r.text(",\"text\":\"");
        const char* hex = "0123456789abcdef";
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t c = g_previous[LastLogHeaderBytes + offset + i];
            if (c == '"' || c == '\\') { r.append('\\'); r.append(static_cast<char>(c)); }
            else if (c < 32 || c >= 127) { r.text("\\u00"); r.append(hex[c >> 4]); r.append(hex[c & 15]); }
            else r.append(static_cast<char>(c));
        }
        r.text("\"}\n");
        r.send();
        return true;
    } else if (strcmp(rest, " status") && *rest) {
        DebugChannelSend("{\"lastlog_error\":\"Usage: lastlog status|on|off|flush|previous OFFSET COUNT\"}\n");
        return true;
    }
    const State state = LoadState();
    Reply r;
    r.text("{\"lastlog_api\":1,\"enabled\":"); r.text(__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE) ? "true" : "false");
    r.text(",\"state\":"); r.quoted(StateName(state));
    r.text(",\"boot_id\":"); r.quoted(g_bootId);
    r.text(",\"slots\":[\""); r.text(kSlots[0]); r.text("\",\""); r.text(kSlots[1]); r.text("\"]");
    r.text(","); r.field("interval_ms", LastLogIntervalMs);
    r.text(","); r.field("next_sequence", g_sequence);
    r.text(","); r.field("writes", g_writes);
    r.text(","); r.field("failures", g_failures);
    r.text(",\"last_error\":"); if (g_lastError < 0) r.text("-"); r.number(static_cast<uint64_t>(g_lastError < 0 ? -g_lastError : g_lastError));
    r.text(",\"prepare_error\":"); if (g_prepareError < 0) r.text("-"); r.number(static_cast<uint64_t>(g_prepareError < 0 ? -g_prepareError : g_prepareError));
    r.text(","); r.field("last_write_ms", g_lastWriteMs);
    r.text(","); r.field("last_bytes", g_lastBytes);
    r.text(","); r.field("now_ms", ApicTickCount());
    r.text(",\"previous\":");
    if (__atomic_load_n(&g_previousPublished, __ATOMIC_ACQUIRE)) RecordJson(r, g_previousRecord);
    else r.text("{\"valid\":false}");
    r.text(",\"scope\":\"record = header + census top 40 + newest serial-tail bytes (superset of the kernel log ring); written at most every interval_ms when serial output or the census changed, fsync per write; previous = newest valid record found before this boot's first write, also copied to previous.bin; boot_id = bridge boot epoch once established, else a random per-boot nonce\"}\n");
    r.send();
    return true;
}

} // namespace brook
