#include "../../drivers/e1000/i219_recovery.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace brook::i219;

namespace {

struct Firmware {
    static constexpr uint32_t Unrelated = 0x10020;
    uint32_t h2me = Unrelated | H2meUlp;
    unsigned clearAfter, sleeps = 0, writes = 0;
    uint32_t history[2]{};

    uint32_t readH2me() const { return h2me; }
    uint32_t readFwsm() const { return FwsmFwValid | (sleeps < clearAfter ? FwsmUlpCfgDone : 0); }
    void writeH2me(uint32_t value) {
        assert(writes < 2);
        history[writes++] = value;
        h2me = value;
    }
    void sleepMs(uint32_t ms) { assert(ms == 10); ++sleeps; }
};

void CheckFirmware()
{
    for (const unsigned delay : {0u, 11u, 250u, 251u}) {
        Firmware firmware{Firmware::Unrelated | H2meUlp, delay};
        assert(RequestFirmwareExit(firmware) == (delay <= 250));
        assert(firmware.sleeps == (delay <= 250 ? delay : 250));
        assert(firmware.writes == 2);
        assert(firmware.history[0] == (Firmware::Unrelated | H2meEnforce));
        assert(firmware.history[1] == Firmware::Unrelated);
    }
    assert(FirmwareUlpBusy(FwsmFwValid | FwsmUlpCfgDone));
    assert(!FirmwareUlpBusy(FwsmFwValid));
    assert(!FirmwareUlpBusy(FwsmUlpCfgDone));
    assert(!HostUlpNeeded(0, 0, UnreadPhy, false));
    assert(HostUlpNeeded(FwsmUlpCfgDone, 0, 0, false));
    assert(HostUlpNeeded(0, H2meUlp, 0, false));
    assert(HostUlpNeeded(0, 0, 0x1000, false));
    assert(HostUlpNeeded(0, 0, UnreadPhy, true));
    assert(ActiveUlpConfig(UlpConfigMask | UlpStart | 0x8000) == 0x8000);
    assert(PhyLinkPowerMask == 0x4e);
    assert(RestartBmcr(BmcrPowerDown | 0x0100) == (BmcrAnEnable | BmcrAnRestart | 0x0100));
}

void CheckLink()
{
    LinkRecovery link;
    link.Start(1'000);
    assert(link.Poll(1'000, true) == LinkAction::Up);
    assert(link.Poll(20'000, false) == LinkAction::None);
    assert(link.restarts == 0);

    link.Start(3'000);
    assert(link.Poll(7'999, false) == LinkAction::None);
    assert(link.Poll(8'000, false) == LinkAction::Restart);
    assert(link.restarts == 1);
    assert(link.Poll(8'001, true) == LinkAction::Up);
    assert(link.Poll(90'000, false) == LinkAction::None);

    link.Start(0);
    assert(link.Poll(5'250, false) == LinkAction::Restart);
    assert(link.Poll(10'249, false) == LinkAction::None);
    assert(link.Poll(10'250, false) == LinkAction::Restart);
    assert(link.Poll(15'250, false) == LinkAction::TimedOut);
    assert(link.restarts == 2 && link.Poll(30'000, false) == LinkAction::None);
    assert(link.Poll(30'001, true) == LinkAction::Up);
}

}

int main()
{
    CheckFirmware();
    CheckLink();
    std::puts("I219 ULP handshake bounds, register masks, and link recovery passed");
}
