// kcmp(2): the real handler over fake tasks. Linux kernel/kcmp.c is the spec.
#include "../../kernel/src/syscall_kcmp.cpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
unsigned failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "KCMP FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
brook::Process* g_tasks[8];
brook::Process* g_self;
bool g_denied[8];
uintptr_t g_undo[8];
unsigned g_refs, g_regionDepth;
uint64_t g_random = 0x1234;
}

namespace brook {
Process* ProcessCurrent() { return g_self; }
Process* SchedulerFindProcessReference(uint16_t pid)
{
    for (auto* t : g_tasks) if (t && t->pid == pid) { ++g_refs; ++t->refCount; return t; }
    return nullptr;
}
bool ProcessCanPtraceRead(Process*, Process* target)
{
    for (unsigned i = 0; i < 8; ++i) if (g_tasks[i] == target) return !g_denied[i];
    return false;
}
uintptr_t SysvSemUndoIdentity(Process* p)
{
    for (unsigned i = 0; i < 8; ++i) if (g_tasks[i] == p) return g_undo[i];
    return 0;
}
void SchedulerKernelRegionEnter(Process*) { ++g_regionDepth; }
void SchedulerKernelRegionLeave(Process*) { --g_regionDepth; }
}
bool RdrandU64(uint64_t*) { return false; }
uint64_t SoftRandU64() { g_random = g_random * 6364136223846793005ULL + 1442695040888963407ULL; return g_random; }

int main()
{
    using namespace brook;
    static Process tasks[4] = {};
    static FdEntry tableA[MAX_FDS], tableB[MAX_FDS];
    static OpenFile shared{}, other{};
    static AddressSpaceState spaceA{}, spaceB{};
    static int fsA, fsB;
    for (unsigned i = 0; i < 4; ++i) {
        tasks[i].magic = PROCESS_MAGIC; tasks[i].pid = tasks[i].tgid = uint16_t(10 + i);
        tasks[i].incarnation = 100 + i; g_tasks[i] = &tasks[i];
    }
    // tasks[0] and its thread tasks[1] share everything; tasks[2] is a fork.
    tasks[1].isThread = true; tasks[1].tgid = 10; tasks[1].leaderIncarnation = 100;
    tasks[1].threadLeader = &tasks[0];
    tasks[0].fds = tasks[1].fds = tableA; tasks[2].fds = tableB; tasks[3].fds = nullptr;
    tasks[0].addressSpace = tasks[1].addressSpace = &spaceA; tasks[2].addressSpace = &spaceB;
    tasks[0].fsContext = tasks[1].fsContext = reinterpret_cast<FsContext*>(&fsA);
    tasks[2].fsContext = reinterpret_cast<FsContext*>(&fsB);
    // fd 3 and its dup fd 4 name one description; the fork inherited it at fd 5.
    tableA[3] = {FdType::Vnode, 0, 0, 0, 0, &fsA, &shared, {}};
    tableA[4] = tableA[3];
    tableA[6] = {FdType::Vnode, 0, 0, 0, 0, &fsA, &other, {}}; // same vnode, separate open()
    tableB[5] = tableA[3];
    tableA[7] = tableA[3]; tableA[7].closing = 1;
    g_self = &tasks[0];

    CHECK(sys_kcmp(10, 10, KcmpFile, 3, 4, 0) == 0);
    CHECK(sys_kcmp(10, 12, KcmpFile, 3, 5, 0) == 0);
    const int64_t order = sys_kcmp(10, 10, KcmpFile, 3, 6, 0);
    CHECK(order == 1 || order == 2);
    CHECK(sys_kcmp(10, 10, KcmpFile, 6, 3, 0) == (order ^ 3)); // antisymmetric
    CHECK(sys_kcmp(10, 10, KcmpFile, 3, 9, 0) == -EBADF);       // free slot
    CHECK(sys_kcmp(10, 10, KcmpFile, 7, 3, 0) == -EBADF);       // closing slot
    CHECK(sys_kcmp(10, 10, KcmpFile, MAX_FDS, 3, 0) == -EBADF);
    CHECK(sys_kcmp(10, 13, KcmpFile, 3, 3, 0) == -EBADF);       // no table
    CHECK(sys_kcmp(10, 11, KcmpFile, 3 + (1ULL << 32), 4, 0) == 0); // idx is unsigned int
    for (uint64_t type : {uint64_t(KcmpVm), uint64_t(KcmpFiles), uint64_t(KcmpFs), uint64_t(KcmpSighand)}) {
        CHECK(sys_kcmp(10, 11, type, 0, 0, 0) == 0);
        const int64_t r = sys_kcmp(10, 12, type, 0, 0, 0);
        CHECK(r == 1 || r == 2);
        CHECK(sys_kcmp(12, 10, type, 0, 0, 0) == (r ^ 3));
    }
    CHECK(sys_kcmp(10, 12, KcmpIo, 0, 0, 0) == 0);
    CHECK(sys_kcmp(10, 12, KcmpSysvsem, 0, 0, 0) == 0); // neither has an undo list
    g_undo[0] = g_undo[1] = 0x5000; g_undo[2] = 0x6000;
    CHECK(sys_kcmp(10, 11, KcmpSysvsem, 0, 0, 0) == 0);
    CHECK(sys_kcmp(10, 12, KcmpSysvsem, 0, 0, 0) != 0);
    CHECK(sys_kcmp(10, 10, KcmpEpollTfd, 3, 0, 0) == -95);
    CHECK(sys_kcmp(10, 10, KcmpTypes, 0, 0, 0) == -EINVAL);
    CHECK(sys_kcmp(10, 10, uint64_t(-1), 0, 0, 0) == -EINVAL);
    // Errors in Linux order: ESRCH, then EPERM, then the type's own errors.
    CHECK(sys_kcmp(10, 99, KcmpTypes, 0, 0, 0) == -ESRCH);
    CHECK(sys_kcmp(0, 10, KcmpVm, 0, 0, 0) == -ESRCH);
    CHECK(sys_kcmp(uint64_t(-10), 10, KcmpVm, 0, 0, 0) == -ESRCH);
    CHECK(sys_kcmp(10, 0x1000a, KcmpVm, 0, 0, 0) == -ESRCH); // no 16-bit aliasing
    g_denied[2] = true;
    CHECK(sys_kcmp(10, 12, KcmpTypes, 0, 0, 0) == -EPERM);
    CHECK(sys_kcmp(12, 10, KcmpFile, 99, 99, 0) == -EPERM);
    // Every lookup reference is dropped and the region is balanced.
    unsigned live = 0;
    for (auto& t : tasks) live += t.refCount;
    CHECK(!live && g_refs > 0 && !g_regionDepth);
    // Obfuscation is a bijection: distinct identities never compare equal.
    KcmpCookies cookies{};
    KcmpCookiesInit(cookies, [] { return SoftRandU64(); });
    for (uint32_t t = 0; t < KcmpTypes; ++t) CHECK(cookies.multiplier[t] & 1);
    CHECK(KcmpOrder(cookies, 0, 0x1000, 0x1000) == 0 && KcmpOrder(cookies, 0, 0x1000, 0x1008) != 0);
    std::printf("kcmp: %u failures\n", failures);
    return failures ? 1 : 0;
}
