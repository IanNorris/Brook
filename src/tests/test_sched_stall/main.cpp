// Actual idle-stall reporter, with deterministic retirement at lock release.
// mprotect makes an unreferenced Process inaccessible at the first legal
// post-unlock opportunity. This tests lifetime, not arbitrary SMP execution.
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include "../../kernel/src/sched_ops.h"

namespace {
using brook::SCHED_PID_NONE;
constexpr unsigned SCHED_MAX_CPUS = 8;
constexpr uint64_t PROCESS_MAGIC = 0x12345678;
enum class ProcessState { Ready, Running, Terminated };
struct Process {
    uint64_t magic = PROCESS_MAGIC, incarnation = 1;
    uint16_t pid = 7;
    ProcessState state = ProcessState::Ready;
    int32_t runningOnCpu = -1;
    bool eligible = true;
    char name[32] = "original";
    uint64_t schedTrace[12]{};
    uint8_t schedTraceHead = 0;
};
struct Cpu { Process* pendingRequeue = nullptr; } g_perCpu[SCHED_MAX_CPUS];
static Process* g_allProcesses[1];
static uint32_t g_processCount = 1;
static int g_readyLock, g_allProcLock;
static bool readyHeld, allHeld, retireOnUnlock, retired;
static unsigned failures, reports, dumps;
static uint64_t tick;
static void* page;
static size_t pageSize;
static char output[4096];
static size_t outputLength;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); ++failures; } } while (0)
static uint64_t ApicTickCount() { return tick; }
static bool ProcessCanRunOnCpu(Process* p, uint32_t) { return p->eligible; }
static uint64_t SchedLockAcquire(int& lock) {
    if (&lock == &g_readyLock) { CHECK(!readyHeld && !allHeld); readyHeld = true; }
    else { CHECK(!allHeld && readyHeld); allHeld = true; }
    return 0;
}
static void SchedLockRelease(int& lock, uint64_t) {
    if (&lock == &g_readyLock) { CHECK(readyHeld && !allHeld); readyHeld = false; }
    else { CHECK(allHeld); allHeld = false; }
    if (!readyHeld && !allHeld && retireOnUnlock && !retired) {
        // A remote kill/wait/reap can now detach and destroy the candidate.
        g_allProcesses[0] = nullptr; g_processCount = 0;
        CHECK(mprotect(page, pageSize, PROT_NONE) == 0);
        retired = true;
    }
}
static void dump(void*, uint16_t pid, brook::SchedDebugInfo* out) {
    CHECK(readyHeld && allHeld && pid == 7); ++dumps;
    out->readyCount = out->listLen = 0;
}
static brook::SchedOps ops;
static const brook::SchedOps* g_schedOps = &ops;
static void* g_schedState;
static void SerialPrintf(const char* format, ...) {
    CHECK(!readyHeld && !allHeld);
    if (std::strstr(format, "SCHED STALL:")) ++reports;
    va_list args; va_start(args, format);
    int n = std::vsnprintf(output + outputLength, sizeof(output)-outputLength, format, args);
    va_end(args);
    CHECK(n >= 0 && static_cast<size_t>(n) < sizeof(output)-outputLength);
    if (n > 0 && static_cast<size_t>(n) < sizeof(output)-outputLength) outputLength += n;
}

#ifdef BROOK_STALL_LEGACY_BODY
#include BROOK_STALL_LEGACY_BODY
#else
#include "../../kernel/src/scheduler_stall.inc"
#endif

static void observe(uint64_t now) { tick = now; ReportPersistentReadyStall(0, 0); }
static int run(unsigned scenario) {
    pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    page = mmap(nullptr, pageSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 2;
    auto* p = new (page) Process{};
    p->schedTrace[0] = (42ULL << 16) | (3 << 8) | 1;
    p->schedTraceHead = 1;
    g_allProcesses[0] = p; ops.DebugDump = dump;
    observe(499); CHECK(!reports && !dumps);
    observe(500); CHECK(!reports && !dumps);
    switch (scenario) {
    case 0: // Delete between snapshot unlock and formatting.
        retireOnUnlock = true; observe(1000);
        CHECK(retired && reports == 1 && dumps == 1);
        CHECK(std::strstr(output, "original") && std::strstr(output, "tick=42"));
        break;
    case 1: // Same PID, different lifetime cannot establish persistence.
        ++p->incarnation; observe(1000); CHECK(!reports && !dumps);
        observe(1500); CHECK(reports == 1 && dumps == 1);
        break;
    case 2: // Pending requeue breaks persistence; later two observations needed.
        g_perCpu[7].pendingRequeue = p; observe(1000); CHECK(!reports);
        g_perCpu[7].pendingRequeue = nullptr; observe(1500); CHECK(!reports);
        observe(2000); CHECK(reports == 1);
        break;
    case 3: // Already running on a remote CPU is not a stranded task.
        p->runningOnCpu = 3; observe(1000); CHECK(!reports && !dumps);
        break;
    case 4:
        p->state = ProcessState::Terminated; observe(1000); CHECK(!reports);
        break;
    case 5:
        p->eligible = false; observe(1000); CHECK(!reports);
        break;
    case 6: // No policy debug callback is a supported diagnostic case.
        ops.DebugDump = nullptr; observe(1000); CHECK(reports == 1 && !dumps);
        break;
    case 7: // Untrusted/missing name terminator cannot escape the local copy.
        std::memset(p->name, 'x', sizeof(p->name));
        observe(1000); CHECK(reports == 1 && dumps == 1);
        break;
    }
    CHECK(!readyHeld && !allHeld);
    CHECK(munmap(page, pageSize) == 0);
    return failures ? 1 : 0;
}
}

int main() {
    // Every child starts with pristine function-static persistence state.
    rlimit noCore{0, 0}; setrlimit(RLIMIT_CORE, &noCore);
    unsigned failed = 0;
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        pid_t child = fork();
        if (child == 0) _exit(run(scenario));
        if (child < 0) return 2;
        int status = 0;
        if (waitpid(child, &status, 0) != child) return 2;
        const bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::printf("SCHED_STALL_CASE %u %s status=%d\n", scenario, passed?"PASS":"FAIL", status);
        failed += !passed;
    }
    std::printf("SCHED_STALL_ACTUAL_BODY cases=8 failed=%u\n", failed);
    return failed ? 1 : 0;
}
