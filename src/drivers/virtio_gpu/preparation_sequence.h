#pragma once
namespace brook {
// The preparation callback may touch only independent CPU storage. Even when
// construction fails, finish the published request before returning. Callers
// must not publish a draw unless this sequence succeeds. Kernel callbacks do
// not throw; this is not an exception-unwinding ownership mechanism.
template<class Begin,class Prepare,class Complete>
bool GpuPrepareWhilePending(Begin begin,Prepare prepare,Complete complete) {
    const auto token=begin();
    if(!token)return false;
    const bool prepared=prepare();
    const bool completed=complete(token);
    return prepared && completed;
}
}
