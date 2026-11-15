// H-08/H-09/H-10/H-19 and M-23: handlers that validate a user range and then
// dereference it fault the kernel if a sibling unmaps the range in between.
// The harness unmaps right after validation; a fixed handler's checked copy
// then fails with -EFAULT and the "unmapped" page is untouched.
#include "harness.h"
#include "syscall_sched.cpp"
#include "syscall_cred.cpp"
#include "syscall_time.cpp"
#include "syscall_signal.cpp"
#include "syscall_sysinfo.cpp"
#include "syscall_poll.h"

using namespace harness;
namespace {
constexpr uint8_t Pattern=0x5a;
uint64_t Patterned(uint64_t offset,uint64_t length) {
    __builtin_memset(reinterpret_cast<void*>(UserAddress(offset)),Pattern,length);
    return UserAddress(offset);
}
bool Untouched(uint64_t address,uint64_t length) {
    for(uint64_t i=0;i<length;++i)if(reinterpret_cast<const uint8_t*>(address)[i]!=Pattern)return false;
    return true;
}
void Begin() { Reset();ResetProcess(); }
// Either the handler has no validate step left to race (it only uses the
// checked copy), or the raced range was never dereferenced.
bool RaceSafe(int64_t result,uint64_t address,uint64_t length) {
    return !g_racedUnmaps || (result==-brook::EFAULT && Untouched(address,length));
}
}

static void SchedGetparam() {
    // H-10: a read-only page passed the readability check and was written.
    Begin();
    Protect(0,PageSize,Access::Read);
    CHECK(brook::sys_sched_getparam(0,UserAddress(0),0,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_sched_getparam(0,0,0,0,0,0)==-brook::EINVAL);
    CHECK(brook::sys_sched_getparam(0,g_kernel.Address(),0,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(brook::sys_sched_getparam(0,UserAddress(PageSize),0,0,0,0)==0);
    // Raced unmap of the affinity mask.
    Begin();g_raceUnmapAfterCheck=true;
    const uint64_t mask=Patterned(0,8);
    CHECK(RaceSafe(brook::sys_sched_getaffinity(0,8,mask,0,0,0),mask,8));
}

static void Credentials() {
    Begin();g_raceUnmapAfterCheck=true;
    // Separate pages, so each validation passes before its own unmap.
    const uint64_t a=Patterned(0,4),b=Patterned(PageSize,4),c=Patterned(2*PageSize,4);
    CHECK(brook::sys_getresuid(a,b,c,0,0,0)==-brook::EFAULT);
    CHECK(Untouched(a,4) && Untouched(b,4) && Untouched(c,4));
}

static void Time() {
    Begin();g_raceUnmapAfterCheck=true;
    // nanosleep reads its request: a raced unmap is EFAULT, not a kernel read.
    const uint64_t request=Patterned(0,16);
    CHECK(!g_racedUnmaps || brook::sys_nanosleep(request,0,0,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_nanosleep(g_kernel.Address(),0,0,0,0,0)==-brook::EFAULT);
    Begin();g_raceUnmapAfterCheck=true;
    const uint64_t value=Patterned(0,32);
    CHECK(brook::sys_timer_gettime(1,value,0,0,0,0)<0);
    CHECK(Untouched(value,32));
    // Kernel destinations are refused outright.
    Begin();
    CHECK(brook::sys_sysinfo(g_kernel.Address(),0,0,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_times(g_kernel.Address(),0,0,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_sched_getaffinity(0,8,g_kernel.Address(),0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
}

static void Signals() {
    // M-23: an invalid how or set must not clobber oldset.
    Begin();auto* proc=CurrentProcess();proc->sigMask=0x1234;
    const uint64_t set=UserAddress(0),old=Patterned(64,8);
    *reinterpret_cast<uint64_t*>(set)=0x10;
    CHECK(brook::sys_rt_sigprocmask(7,set,old,8,0,0)==-brook::EINVAL);
    CHECK(Untouched(old,8));
    CHECK(brook::sys_rt_sigprocmask(0,g_kernel.Address(),old,8,0,0)==-brook::EFAULT);
    CHECK(Untouched(old,8));
    CHECK(brook::sys_rt_sigprocmask(0,set,old,8,0,0)==0);
    CHECK(*reinterpret_cast<const uint64_t*>(old)==0x1234 && proc->sigMask==(0x1234|0x10));
    // Raced unmap of oldset.
    Begin();g_raceUnmapAfterCheck=true;
    const uint64_t raced=Patterned(0,8);
    CHECK(RaceSafe(brook::sys_rt_sigprocmask(0,0,raced,8,0,0),raced,8));
}

static void SystemInfo() {
    Begin();g_raceUnmapAfterCheck=true;
    const uint64_t info=Patterned(0,112);
    CHECK(RaceSafe(brook::sys_sysinfo(info,0,0,0,0,0),info,112));
    Begin();g_raceUnmapAfterCheck=true;
    const uint64_t usage=Patterned(0,144);
    CHECK(RaceSafe(brook::sys_getrusage(0,usage,0,0,0,0),usage,144));
    Begin();g_raceUnmapAfterCheck=true;
    const uint64_t times=Patterned(0,32);
    CHECK(RaceSafe(brook::sys_times(times,0,0,0,0,0),times,32));
    Begin();
    CHECK(brook::sys_getrlimit(7,g_kernel.Address(),0,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_prlimit64(0,7,0,g_kernel.Address(),0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
}

static void Poll() {
    // The pollfd array is snapshotted and written back with checked copies.
    Begin();g_raceUnmapAfterCheck=true;
    const uint64_t fds=Patterned(0,8);
    CHECK(RaceSafe(brook::sys_poll(fds,1,0,0,0,0),fds,8));
}

void RunValidateRaceTests() {
    SchedGetparam();
    Credentials();
    Time();
    Signals();
    SystemInfo();
    Poll();
}
