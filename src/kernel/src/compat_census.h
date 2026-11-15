#pragma once
// compat_census.h -- bounded, lock-free census of Linux-compatibility gaps.
//
// Records which syscalls return ENOSYS and which sub-operations of known
// dispatchers (ioctl requests, fcntl commands, prctl options, socket options,
// clone flags, socket families, mmap/madvise flags, missing /proc, /sys and
// /dev paths) are refused, so the gaps real Linux software hits stay visible
// after boot. Only failing syscalls reach this code; the success path pays a
// single range compare in the dispatcher.
//
// The table is fixed-size open addressing. A slot is claimed with one CAS
// (Empty -> Writing), filled, then published (Ready). Matching updates are
// relaxed atomic adds. No lock is taken, nothing allocates, and an insert
// that cannot find a slot within the probe bound only bumps an overflow
// counter. Pure and host-testable: callers supply the tick and identity.
#include <stdint.h>

namespace brook {

enum class CompatKind : uint8_t {
    Syscall = 1,     // nr = syscall, a = origin (CompatSyscallOrigin)
    Ioctl,           // a = FdType, b = request
    Fcntl,           // b = cmd
    Prctl,           // b = option
    ArchPrctl,       // b = code
    Sockopt,         // nr = setsockopt/getsockopt, a = level, b = optname
    Socket,          // nr = socket/socketpair, a = family, b = type, c = protocol
    Clone,           // nr = clone/clone3, b = flags
    Mmap,            // nr = mmap/mremap/madvise/mprotect, a = prot/0, b = flags/advice
    Path,            // nr = path syscall, text = normalized path
    ProcessName,     // text = process name (per-process ENOSYS/unsupported totals)
};

enum CompatSyscallOrigin : uint64_t {
    CompatOriginHandler = 0,     // an implemented handler returned ENOSYS
    CompatOriginUnimplemented = 1,
    CompatOriginQuiet = 2,       // deliberately unimplemented, silent fallback
    CompatOriginOutOfRange = 3,
};

inline const char* CompatKindName(CompatKind kind)
{
    switch (kind) {
    case CompatKind::Syscall: return "syscall";
    case CompatKind::Ioctl: return "ioctl";
    case CompatKind::Fcntl: return "fcntl";
    case CompatKind::Prctl: return "prctl";
    case CompatKind::ArchPrctl: return "arch_prctl";
    case CompatKind::Sockopt: return "sockopt";
    case CompatKind::Socket: return "socket";
    case CompatKind::Clone: return "clone";
    case CompatKind::Mmap: return "mmap";
    case CompatKind::Path: return "path";
    case CompatKind::ProcessName: return "process";
    }
    return "?";
}

static constexpr uint32_t CompatTextBytes = 48;
static constexpr uint32_t CompatNameBytes = 16;

struct CompatKey {
    CompatKind kind = CompatKind::Syscall;
    uint16_t error = 0;          // positive errno
    uint32_t nr = 0;
    uint64_t a = 0, b = 0, c = 0;
    uint64_t textHash = 0;       // 0 when the key has no text
};

inline bool CompatKeyEqual(const CompatKey& x, const CompatKey& y)
{
    return x.kind == y.kind && x.error == y.error && x.nr == y.nr && x.a == y.a &&
           x.b == y.b && x.c == y.c && x.textHash == y.textHash;
}

inline uint64_t CompatMix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 33);
}

inline uint64_t CompatTextHash(const char* text)
{
    if (!text || !*text) return 0;
    uint64_t h = 0xcbf29ce484222325ull;            // FNV-1a
    for (uint32_t i = 0; i < CompatTextBytes - 1 && text[i]; ++i) {
        h ^= static_cast<unsigned char>(text[i]);
        h *= 0x100000001b3ull;
    }
    return h ? h : 1;
}

inline uint64_t CompatKeyHash(const CompatKey& k)
{
    uint64_t h = CompatMix(static_cast<uint64_t>(k.kind) << 16 | k.error, k.nr);
    h = CompatMix(h, k.a);
    h = CompatMix(h, k.b);
    h = CompatMix(h, k.c);
    return CompatMix(h, k.textHash);
}

enum : uint32_t { CompatSlotEmpty = 0, CompatSlotWriting = 1, CompatSlotReady = 2 };

struct CompatEntry {
    uint32_t state;              // CompatSlot*
    uint32_t firstPid;
    CompatKey key;
    uint64_t count;
    uint64_t firstTick, lastTick;
    char firstName[CompatNameBytes];
    char text[CompatTextBytes];
};

inline void CompatCopyText(char* destination, uint32_t capacity, const char* source)
{
    uint32_t i = 0;
    if (source)
        for (; i + 1 < capacity && source[i]; ++i) destination[i] = source[i];
    for (; i < capacity; ++i) destination[i] = 0;
}

// Bounded text builder (no printf in the kernel's syscall paths).
struct CompatText {
    char* out;
    uint32_t capacity;
    uint32_t used = 0;
    CompatText(char* o, uint32_t c) : out(o), capacity(c) { if (capacity) out[0] = 0; }
    void str(const char* s)
    {
        while (s && *s && used + 1 < capacity) out[used++] = *s++;
        if (capacity) out[used] = 0;
    }
    void num(uint64_t v, bool hex = false)
    {
        char d[24];
        uint32_t k = 0;
        const uint32_t base = hex ? 16 : 10;
        do { const uint32_t x = static_cast<uint32_t>(v % base); d[k++] = static_cast<char>(x < 10 ? '0' + x : 'a' + x - 10); v /= base; } while (v);
        if (hex) str("0x");
        while (k && used + 1 < capacity) out[used++] = d[--k];
        if (capacity) out[used] = 0;
    }
};

// One line describing a census entry, e.g. for logs:
// "ioctl nr=16 err=25 a=0x4 b=0x5401 c=0x0 count=3 first=bash"
inline void CompatDescribe(CompatText& t, const CompatKey& key, const char* text,
                           uint64_t count, const char* firstName)
{
    t.str(CompatKindName(key.kind)); t.str(" nr="); t.num(key.nr); t.str(" err="); t.num(key.error);
    t.str(" a="); t.num(key.a, true); t.str(" b="); t.num(key.b, true); t.str(" c="); t.num(key.c, true);
    if (text && *text) { t.str(" path="); t.str(text); }
    if (count) { t.str(" count="); t.num(count); }
    if (firstName) { t.str(" first="); t.str(firstName); }
}

enum class CompatNote : uint8_t { Counted, New, Overflow };

// Deliberate ENOSYS with a userspace fallback (sys_enosys_quiet): counted,
// but reported apart so they cannot crowd out real gaps.
inline bool CompatKeyQuiet(const CompatKey& key)
{
    return key.kind == CompatKind::Syscall && key.a == CompatOriginQuiet;
}
enum class CompatSelect : uint8_t { All, Loud, Quiet };

template<uint32_t Capacity, uint32_t MaxProbe = 32>
struct CompatCensusTable {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(MaxProbe <= Capacity, "probe bound exceeds capacity");
    CompatEntry entries[Capacity];
    uint64_t overflow;           // inserts dropped: no free slot within MaxProbe
    uint64_t total;              // every observation, counted or dropped
    uint64_t resets;

    // Count one observation. Returns New exactly once per key (barring a
    // rare duplicate when two CPUs race to create the same key and one gives
    // up waiting), Counted on later hits and Overflow when the table is full.
    CompatNote Note(const CompatKey& key, const char* text, uint64_t tick,
                    uint32_t pid, const char* name)
    {
        __atomic_fetch_add(&total, 1, __ATOMIC_RELAXED);
        const uint64_t hash = CompatKeyHash(key);
        for (uint32_t probe = 0; probe < MaxProbe; ++probe) {
            CompatEntry& e = entries[(hash + probe) & (Capacity - 1)];
            uint32_t state = __atomic_load_n(&e.state, __ATOMIC_ACQUIRE);
            if (state == CompatSlotEmpty) {
                uint32_t expected = CompatSlotEmpty;
                // Acquire on failure too: a lost race may observe Ready and
                // then compare the published key.
                if (__atomic_compare_exchange_n(&e.state, &expected, CompatSlotWriting, false,
                                                __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
                    e.key = key;
                    e.firstPid = pid;
                    e.firstTick = tick;
                    __atomic_store_n(&e.lastTick, tick, __ATOMIC_RELAXED);
                    __atomic_store_n(&e.count, 1, __ATOMIC_RELAXED);
                    CompatCopyText(e.firstName, CompatNameBytes, name);
                    CompatCopyText(e.text, CompatTextBytes, text);
                    __atomic_store_n(&e.state, CompatSlotReady, __ATOMIC_RELEASE);
                    return CompatNote::New;
                }
                state = expected;
            }
            // Another CPU is filling this slot; it publishes within a few
            // dozen stores. Bounded wait, then treat it as a different key.
            for (uint32_t spin = 0; state == CompatSlotWriting && spin < 1024; ++spin) {
#if defined(__x86_64__)
                __asm__ volatile("pause");
#endif
                state = __atomic_load_n(&e.state, __ATOMIC_ACQUIRE);
            }
            if (state == CompatSlotReady && CompatKeyEqual(e.key, key)) {
                __atomic_fetch_add(&e.count, 1, __ATOMIC_RELAXED);
                __atomic_store_n(&e.lastTick, tick, __ATOMIC_RELAXED);
                return CompatNote::Counted;
            }
        }
        __atomic_fetch_add(&overflow, 1, __ATOMIC_RELAXED);
        return CompatNote::Overflow;
    }

    // Clear every published slot. Concurrent writers may lose a count or
    // land one in a slot being cleared; both are acceptable for a census.
    void Reset()
    {
        for (uint32_t i = 0; i < Capacity; ++i) {
            CompatEntry& e = entries[i];
            uint32_t expected = CompatSlotReady;
            if (!__atomic_compare_exchange_n(&e.state, &expected, CompatSlotWriting, false,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                continue;
            __atomic_store_n(&e.count, 0, __ATOMIC_RELAXED);
            e.key = CompatKey{};
            __atomic_store_n(&e.state, CompatSlotEmpty, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&overflow, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&total, 0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&resets, 1, __ATOMIC_RELAXED);
    }

    // Copy published entries into out[] (at most limit), sorted by count
    // descending, optionally only the loud or quiet set. Returns the number
    // copied; *used receives the number of published slots (all sets).
    // Readers never block writers.
    uint32_t Snapshot(CompatEntry* out, uint32_t limit, uint32_t* used = nullptr,
                      CompatSelect select = CompatSelect::All) const
    {
        uint32_t n = 0, ready = 0;
        for (uint32_t i = 0; i < Capacity; ++i) {
            const CompatEntry& e = entries[i];
            if (__atomic_load_n(&e.state, __ATOMIC_ACQUIRE) != CompatSlotReady) continue;
            ++ready;
            CompatEntry copy;
            copy.state = CompatSlotReady;
            copy.firstPid = e.firstPid;
            copy.key = e.key;
            copy.count = __atomic_load_n(&e.count, __ATOMIC_RELAXED);
            copy.firstTick = e.firstTick;
            copy.lastTick = __atomic_load_n(&e.lastTick, __ATOMIC_RELAXED);
            for (uint32_t j = 0; j < CompatNameBytes; ++j) copy.firstName[j] = e.firstName[j];
            for (uint32_t j = 0; j < CompatTextBytes; ++j) copy.text[j] = e.text[j];
            copy.firstName[CompatNameBytes - 1] = 0;
            copy.text[CompatTextBytes - 1] = 0;
            if (!copy.count) continue;
            if (select != CompatSelect::All &&
                CompatKeyQuiet(copy.key) != (select == CompatSelect::Quiet)) continue;
            // Insertion into a bounded top-N list.
            uint32_t at = n < limit ? n : limit;
            while (at > 0 && out[at - 1].count < copy.count) {
                if (at < limit) out[at] = out[at - 1];
                --at;
            }
            if (at < limit) {
                out[at] = copy;
                if (n < limit) ++n;
            }
        }
        if (used) *used = ready;
        return n;
    }
};

// Normalize a path for the census: keep it only if it lies under /proc,
// /sys or /dev, and replace all-digit components (PIDs, TIDs, fds, CPU
// numbers stay as written only when mixed with letters) with "N" so one
// missing per-process file is one entry. Returns false for other paths.
inline bool CompatNormalizePath(const char* path, char* out, uint32_t capacity)
{
    auto prefixed = [path](const char* p) {
        uint32_t i = 0;
        for (; p[i]; ++i)
            if (path[i] != p[i]) return false;
        return path[i] == 0 || path[i] == '/';
    };
    if (!path || !(prefixed("/proc") || prefixed("/sys") || prefixed("/dev"))) return false;
    uint32_t o = 0, i = 0;
    while (path[i] && o + 1 < capacity) {
        if (path[i] == '/' && path[i + 1] >= '0' && path[i + 1] <= '9') {
            uint32_t j = i + 1;
            while (path[j] >= '0' && path[j] <= '9') ++j;
            if (path[j] == 0 || path[j] == '/') {
                if (o + 2 >= capacity) break;
                out[o++] = '/';
                out[o++] = 'N';
                i = j;
                continue;
            }
        }
        out[o++] = path[i++];
    }
    out[o] = 0;
    return true;
}

} // namespace brook
