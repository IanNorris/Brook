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
}
