// Host tests for futex_waitv argument policy, its restart class and the
// RLIMIT_NOFILE admission policy (the real kernel headers).
#include <cstdio>
#include <initializer_list>
#include "futex_waitv.h"
#include "resource_limits.h"
#include "signal_restart.h"

static int g_failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "test_futex_waitv:%d: %s\n", __LINE__, #x); ++g_failures; } } while (0)

using namespace brook;

static void CallChecks()
{
    CHECK(FutexWaitvCheckCall(0x1000, 1, 0, 0, 0) == 0);
    CHECK(FutexWaitvCheckCall(0x1000, 128, 0, 0, 99) == 0);   // clock ignored without timeout
    CHECK(FutexWaitvCheckCall(0x1000, 1, 1, 0, 1) == -22);    // flags reserved
    CHECK(FutexWaitvCheckCall(0x1000, 0, 0, 0, 1) == -22);    // empty vector
    CHECK(FutexWaitvCheckCall(0x1000, 129, 0, 0, 1) == -22);  // > FUTEX_WAITV_MAX
    CHECK(FutexWaitvCheckCall(0, 1, 0, 0, 1) == -22);         // NULL waiters
    // Proton's probe: futex_waitv(NULL, 0, 0, NULL, 0) must not be ENOSYS.
    CHECK(FutexWaitvCheckCall(0, 0, 0, 0, 0) == -22);
    CHECK(FutexWaitvCheckCall(0x1000, 1, 0, 0x2000, 0) == 0); // CLOCK_REALTIME
    CHECK(FutexWaitvCheckCall(0x1000, 1, 0, 0x2000, 1) == 0); // CLOCK_MONOTONIC
    CHECK(FutexWaitvCheckCall(0x1000, 1, 0, 0x2000, 7) == -22);
    CHECK(FutexWaitvCheckCall(0x1000, 1, 0, 0x2000, 4) == -22); // MONOTONIC_RAW refused
}

static void EntryChecks()
{
    FutexWaitvEntry e{5, 0x1000, FUTEX2_SIZE_U32, 0};
    CHECK(FutexWaitvCheckEntry(e) == 0);
    e.flags = FUTEX2_SIZE_U32 | FUTEX2_PRIVATE; CHECK(FutexWaitvCheckEntry(e) == 0);
    for (uint32_t size : {FUTEX2_SIZE_U8, FUTEX2_SIZE_U16, FUTEX2_SIZE_U64}) {
        e.flags = size; CHECK(FutexWaitvCheckEntry(e) == -22);
        e.flags = size | FUTEX2_PRIVATE; CHECK(FutexWaitvCheckEntry(e) == -22);
    }
    e.flags = FUTEX2_SIZE_U32 | FUTEX2_NUMA; CHECK(FutexWaitvCheckEntry(e) == -22);
    e.flags = FUTEX2_SIZE_U32 | FUTEX2_MPOL; CHECK(FutexWaitvCheckEntry(e) == -22);
    e.flags = FUTEX2_SIZE_U32 | 0x100; CHECK(FutexWaitvCheckEntry(e) == -22);
    e.flags = FUTEX2_SIZE_U32; e.reserved = 1; CHECK(FutexWaitvCheckEntry(e) == -22);
    e.reserved = 0; e.val = 0xffffffffull; CHECK(FutexWaitvCheckEntry(e) == 0);
    e.val = 0x100000000ull; CHECK(FutexWaitvCheckEntry(e) == -22);
    e.val = 0;
    for (uint64_t a : {0x1001ull, 0x1002ull, 0x1003ull}) { e.uaddr = a; CHECK(FutexWaitvCheckEntry(e) == -22); }
    e.uaddr = 0x1004; CHECK(FutexWaitvCheckEntry(e) == 0);
}

static void WokenIndex()
{
    bool queued[8] = {true, true, true, true, true, true, true, true};
    auto q = [&](uint32_t i) { return queued[i]; };
    CHECK(FutexWaitvWokenIndex(8, q) == -1);
    queued[3] = false; CHECK(FutexWaitvWokenIndex(8, q) == 3);
    queued[6] = false; CHECK(FutexWaitvWokenIndex(8, q) == 6);  // Linux: highest woken
    CHECK(FutexWaitvWokenIndex(5, q) == 3);                     // only linked prefix counts
    CHECK(FutexWaitvWokenIndex(0, q) == -1);
}

static void Restart()
{
    // ERESTARTSYS class: absolute timeout, so even timed waits replay.
    CHECK(SignalSyscallMayRestart(449, 0, 0));
    CHECK(SignalSyscallMayRestart(449, 1, 0x1234));
    CHECK(!SignalSyscallMayRestart(448, 0, 0));
    CHECK(!SignalSyscallMayRestart(450, 0, 0));
}

static void Limits()
{
    const auto d = ResourceLimitDefaultNofile();
    CHECK(d.soft == 1024 && d.hard == 524288);
    // Proton: raise soft to hard.
    CHECK(ResourceLimitAdmit(RlimitNofile, d, {d.hard, d.hard}, false) == 0);
    CHECK(ResourceLimitAdmit(RlimitNofile, d, {512, 1024}, false) == 0);     // lower both
    CHECK(ResourceLimitAdmit(RlimitNofile, {512, 1024}, {512, 2048}, false) == -1); // raise hard
    CHECK(ResourceLimitAdmit(RlimitNofile, {512, 1024}, {512, 2048}, true) == 0);
    CHECK(ResourceLimitAdmit(RlimitNofile, d, {d.hard + 1, d.hard}, false) == -22);
    CHECK(ResourceLimitAdmit(RlimitNofile, d, {1024, RlimitNrOpen}, true) == 0);
    CHECK(ResourceLimitAdmit(RlimitNofile, d, {1024, RlimitNrOpen + 1}, true) == -1);
    CHECK(ResourceLimitAdmit(RlimitNofile, d, {1024, ~0ull}, true) == -1);  // RLIM_INFINITY > nr_open
    // RLIMIT_CORE keeps its existing infinite default and has no nr_open cap.
    const CoreResourceLimits core{};
    CHECK(ResourceLimitAdmit(RlimitCore, core, {0, ~0ull}, false) == 0);
    CHECK(ResourceLimitAdmit(RlimitCore, {0, 0}, {0, 1}, false) == -1);
}

int main()
{
    CallChecks();
    EntryChecks();
    WokenIndex();
    Restart();
    Limits();
    if (g_failures) { std::fprintf(stderr, "test_futex_waitv: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("test_futex_waitv: ALL PASS\n");
    return 0;
}
