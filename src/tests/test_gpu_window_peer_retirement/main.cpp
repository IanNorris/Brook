#include "gpu_window_peer_retirement.h"

#include <cassert>

using brook::GpuWindowPeerRetirement;
using Result = GpuWindowPeerRetirement::Result;

int main()
{
    constexpr uint64_t renderer = 41;
    GpuWindowPeerRetirement first{}, next{};
    assert(!first.Retired(renderer) && !first.Retired(0));
    assert(!first.ClaimRestart(0));
    assert(first.Record(0) == Result::WrongRenderer);

    assert(first.Record(renderer) == Result::Recorded);
    assert(first.Record(renderer) == Result::Recorded);
    assert(first.Record(renderer + 1) == Result::WrongRenderer);
    assert(first.Retired(renderer) && !first.Retired(renderer + 1));
    assert(!first.ClaimRestart(renderer));

    // A previously retired connection cannot restart a healthy renderer
    // after it begins serving a different connection.
    assert(!next.Retired(renderer));
    assert(!first.ClaimRestart(renderer));
    assert(next.ClaimRestart(renderer));
    assert(!next.ClaimRestart(renderer));
    assert(next.Record(renderer) == Result::RestartClaimed);
    assert(!next.Retired(renderer));
    assert(first.Retired(renderer));

    // A different desktop process, including one that receives the socket
    // via SCM_RIGHTS or fork, cannot borrow the boot provider's authority.
    assert(brook::GpuWindowRestartProvider(17,17,17,true,false));
    assert(!brook::GpuWindowRestartProvider(18,17,17,true,false));
    assert(!brook::GpuWindowRestartProvider(18,18,17,true,false));
    assert(!brook::GpuWindowRestartProvider(17,17,0,true,false)); // revoked at exec
    assert(!brook::GpuWindowRestartProvider(17,17,17,false,false));
    assert(brook::GpuWindowRestartProvider(17,17,0,false,true));
    assert(!brook::GpuWindowRestartProvider(17,18,0,false,true));

    using Restart = GpuWindowPeerRetirement::Restart;
    GpuWindowPeerRetirement abandoned{}, occupied{}, observed{};
    assert(abandoned.CheckRestart(renderer,false,false,0)==Restart::NoOwner);
    assert(abandoned.CheckRestart(renderer,true,false,0)==Restart::Ready);
    assert(abandoned.ClaimRestart(renderer));
    assert(abandoned.CheckRestart(renderer,true,false,0)==Restart::AlreadySettled);
    assert(occupied.CheckRestart(renderer,true,true,0)==Restart::Busy);
    assert(occupied.CheckRestart(renderer,true,true,BROOK_GPU_WINDOW_PEER_RECOVERY_GRACE_MS-1)==Restart::Busy);
    assert(occupied.CheckRestart(renderer,true,true,BROOK_GPU_WINDOW_PEER_RECOVERY_GRACE_MS)==Restart::Ready);
    assert(observed.CheckRestart(renderer,true,true,1)==Restart::Busy);
    assert(observed.CheckRestart(renderer,true,false,2)==Restart::Ready);
    assert(observed.Record(renderer)==Result::Recorded);
    assert(observed.CheckRestart(renderer,true,true,3)==Restart::AlreadySettled);
}
