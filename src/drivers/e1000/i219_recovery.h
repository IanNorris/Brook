#pragma once

#include <stdint.h>

namespace brook::i219 {

constexpr uint32_t FwsmUlpCfgDone = 1u << 10, FwsmFwValid = 1u << 15;
constexpr uint32_t H2meUlp = 1u << 11, H2meEnforce = 1u << 12;
constexpr uint16_t UlpStart = 0x0001;
constexpr uint16_t UlpConfigMask = 0x0004 | 0x0010 | 0x0020 | 0x0040 | 0x0100 | 0x0400 | 0x0800 | 0x1000;
constexpr uint32_t PhyD0aLplu = 1u << 1, PhyNonD0aLplu = 1u << 2;
constexpr uint32_t PhyNonD0aGbeDisable = 1u << 3, PhyGbeDisable = 1u << 6;
constexpr uint32_t PhyLinkPowerMask = PhyD0aLplu | PhyNonD0aLplu | PhyNonD0aGbeDisable | PhyGbeDisable;
constexpr uint16_t BmcrPowerDown = 1u << 11, BmcrAnEnable = 1u << 12, BmcrAnRestart = 1u << 9;
constexpr uint32_t UnreadPhy = 0x10000;

inline bool FirmwareUlpBusy(uint32_t fwsm)
{
    return (fwsm & (FwsmFwValid | FwsmUlpCfgDone)) == (FwsmFwValid | FwsmUlpCfgDone);
}

inline bool HostUlpNeeded(uint32_t fwsm, uint32_t h2me, uint32_t ulp, bool silent)
{
    return silent || (fwsm & FwsmUlpCfgDone) || (h2me & H2meUlp) || (ulp & UlpConfigMask);
}

inline uint16_t RestartBmcr(uint16_t previous)
{
    return (previous & ~BmcrPowerDown) | BmcrAnEnable | BmcrAnRestart;
}

inline uint16_t ActiveUlpConfig(uint16_t previous)
{
    return static_cast<uint16_t>(previous & ~(UlpConfigMask | UlpStart));
}

// Intel e1000e's forced firmware exit. Deassert our request on a timeout
// as well, so the driver does not leave a pending ENFORCE behind.
template<class Io>
bool RequestFirmwareExit(Io& io)
{
    io.writeH2me((io.readH2me() & ~H2meUlp) | H2meEnforce);
    unsigned polls = 0;
    while ((io.readFwsm() & FwsmUlpCfgDone) && polls < 250) {
        io.sleepMs(10);
        ++polls;
    }
    const bool complete = !(io.readFwsm() & FwsmUlpCfgDone);
    io.writeH2me(io.readH2me() & ~H2meEnforce);
    return complete;
}

enum class LinkAction : uint8_t { None, Restart, Up, TimedOut };

struct LinkRecovery {
    static constexpr uint64_t WaitMs = 5000;
    static constexpr unsigned MaxRestarts = 2;
    uint64_t start = 0, next = 0;
    unsigned restarts = 0;
    bool up = false, timedOut = false;

    void Start(uint64_t now) { *this = {now, now + WaitMs, 0, false, false}; }

    LinkAction Poll(uint64_t now, bool linkUp)
    {
        if (up) return LinkAction::None;
        if (linkUp) {
            up = true;
            return LinkAction::Up;
        }
        if (timedOut || now < next) return LinkAction::None;
        if (restarts == MaxRestarts) {
            timedOut = true;
            return LinkAction::TimedOut;
        }
        ++restarts;
        next = now + WaitMs;
        return LinkAction::Restart;
    }
};

}
