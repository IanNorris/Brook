#include "../../kernel/src/tty_framebuffer_gate.h"

#include <cassert>
#include <future>
#include <thread>

using namespace brook;
using Access = TtyFramebufferGate::Access;

static void DrainPreexistingReaders()
{
    TtyFramebufferGate gate;
    std::promise<void> entered, release;
    auto waitEntered = entered.get_future();
    auto waitRelease = release.get_future();
    std::thread console([&] {
        assert(gate.TryEnter(Access::Draw));
        entered.set_value();
        waitRelease.wait();
        gate.Leave(Access::Draw);
    });
    waitEntered.wait();

    // Legacy draws could already overlap. Closing admission must count both.
    assert(gate.TryEnter(Access::Draw));
    assert(gate.TryClose());
    assert(!gate.TryClose());
    assert(!gate.Drained());
    assert(!gate.TryEnter(Access::Draw));
    assert(!gate.TryEnter(Access::Read));
    assert(!gate.TryEnter(Access::Legacy));
    gate.Leave(Access::Draw);
    assert(!gate.Drained());
    release.set_value();
    console.join();
    assert(gate.Drained());

    gate.MakeManaged();
    gate.Open();
    assert(gate.TryEnter(Access::Read));
    assert(gate.TryEnter(Access::Draw));
    assert(!gate.TryEnter(Access::Draw));
    assert(!gate.TryEnter(Access::Legacy));
    assert(gate.TryClose());
    gate.Leave(Access::Draw);
    assert(!gate.Drained());
    gate.Leave(Access::Read);
    assert(gate.Drained());
    gate.Open();

    assert(gate.TryEnter(Access::Draw));
    gate.Leave(Access::Draw);
    assert(!gate.TryEnter(Access::Legacy));
}

static void AbandonedUpdate()
{
    TtyFramebufferGate gate;
    assert(gate.TryEnter(Access::Legacy));
    assert(gate.TryClose());
    assert(!gate.Drained());
    gate.Open();
    assert(gate.TryEnter(Access::Legacy));
    gate.Leave(Access::Legacy);
    gate.Leave(Access::Legacy);
    assert(gate.TryClose());
    assert(gate.Drained());
    gate.Open();
    assert(!gate.IsManaged());
}

static void ClipStaleRegion()
{
    constexpr auto outside = TtyClipRegion(640, 480, 2000, UINT32_MAX, UINT32_MAX, 3000);
    static_assert(outside.x == 640 && outside.y == 480 && !outside.width && !outside.height);
    constexpr auto partial = TtyClipRegion(640, 480, 635, 470, UINT32_MAX, 30);
    static_assert(partial.x == 635 && partial.y == 470 && partial.width == 5 && partial.height == 10);
    constexpr auto whole = TtyClipRegion(640, 480, 0, 0, 0, 0);
    static_assert(whole.width == 640 && whole.height == 480);
    constexpr auto remainder = TtyClipRegion(640, 480, 30, 60, 0, 0);
    static_assert(remainder.width == 610 && remainder.height == 420);
}

int main()
{
    DrainPreexistingReaders();
    AbandonedUpdate();
    ClipStaleRegion();
}
