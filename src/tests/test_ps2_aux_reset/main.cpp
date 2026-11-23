// Models an i8042 with a PS/2 touchpad to check the auxiliary reset timing
// (int93 regression: the Dell XPS 13's touchpad self-test outlasted a wait
// capped at 100000 status reads, and the aborted init left the late BAT bytes
// in the 8042 output buffer, blocking the keyboard).
#include "ps2_aux_reset.h"
#include <cassert>
#include <cstdio>
#include <deque>

using namespace brook;

struct Sim8042 {
    // Timing model
    uint64_t us = 0;            // virtual time
    uint64_t readUs = 1;        // cost of one port read (i8042/EC ~1 us; SMM emulation more)
    bool clockStuck = false;
    // Auxiliary device model
    bool device = true;
    uint64_t ackMs = 10, batMs = 1500;
    bool batNever = false;
    uint8_t bat = 0xAA;
    // Controller state
    struct Byte { uint8_t value; bool aux; uint64_t readyUs; };
    std::deque<Byte> out;
    bool auxEnabled = true, nextAux = false, nextCfg = false;
    uint8_t cfg = 0x47;
    uint64_t reads = 0;

    bool Ready() const { return !out.empty() && out.front().readyUs <= us; }
    uint8_t In(uint16_t port) {
        us += readUs; ++reads;
        if (port == 0x64) {
            uint8_t status = 0;
            if (Ready()) status |= 1 | (out.front().aux ? 0x20 : 0);
            return status;
        }
        if (!Ready()) return 0;
        const uint8_t v = out.front().value;
        out.pop_front();
        return v;
    }
    void Queue(uint8_t v, bool aux, uint64_t readyUs) {
        if (aux && !auxEnabled) return;
        // keep queue ordered by ready time
        auto it = out.begin();
        while (it != out.end() && it->readyUs <= readyUs) ++it;
        out.insert(it, {v, aux, readyUs});
    }
    void Out(uint16_t port, uint8_t v) {
        if (port == 0x64) {
            if (v == 0xD4) nextAux = true;
            else if (v == 0xA7) {
                auxEnabled = false;
                for (auto it = out.begin(); it != out.end();) it = (it->aux && it->readyUs > us) ? out.erase(it) : it + 1;
            } else if (v == 0xA8) auxEnabled = true;
            else if (v == 0x20) Queue(cfg, false, us);
            else if (v == 0x60) nextCfg = true;
            return;
        }
        if (nextCfg) { cfg = v; nextCfg = false; return; }
        if (nextAux && v == 0xFF && device && auxEnabled) {
            Queue(0xFA, true, us + ackMs * 1000);
            if (!batNever) {
                Queue(bat, true, us + batMs * 1000);
                if (bat == 0xAA) Queue(0x00, true, us + (batMs + 1) * 1000);
            }
        }
        nextAux = false;
    }
    uint64_t NowMs() { return clockStuck ? 0 : us / 1000; }
    void Pause() { us += 0; }
};

static Ps2AuxResetOutcome Run(Sim8042& s, uint64_t& elapsedMs)
{
    const uint64_t t0 = s.us;
    const auto r = Ps2AuxResetDevice(s);
    elapsedMs = (s.us - t0) / 1000;
    return r;
}

int main()
{
    uint64_t ms = 0;
    {   // Dell-like: fast i8042 reads, touchpad BAT after 1.5 s.
        Sim8042 s;
        auto r = Run(s, ms);
        assert(r.result == Ps2AuxReset::Ok && r.selfTest == 0xAA && r.deviceId == 0);
        assert(ms >= 1500 && ms < 1600);
        // The int93 bug: a 100000-read cap was only ~100 ms here.
        assert(s.reads > 100000);
    }
    {   // Slow pad under SMM-emulated 8042 reads.
        Sim8042 s; s.readUs = 5; s.batMs = 3500;
        auto r = Run(s, ms);
        assert(r.result == Ps2AuxReset::Ok);
    }
    {   // No auxiliary device: give up after the ACK window.
        Sim8042 s; s.device = false; s.readUs = 5;
        auto r = Run(s, ms);
        assert(r.result == Ps2AuxReset::NoDevice);
        assert(ms >= 200 && ms < 260);
    }
    {   // Self-test failure byte.
        Sim8042 s; s.bat = 0xFC; s.batMs = 300;
        auto r = Run(s, ms);
        assert(r.result == Ps2AuxReset::SelfTestFailed && r.selfTest == 0xFC);
    }
    {   // ACK but no BAT: bounded by the 4 s BAT window.
        Sim8042 s; s.batNever = true;
        auto r = Run(s, ms);
        assert(r.result == Ps2AuxReset::Timeout);
        assert(ms >= 4000 && ms < 4100);
    }
    {   // BAT later than the window: after Ps2AuxDisable the late bytes must
        // not occupy the output buffer; a keyboard byte gets through.
        Sim8042 s; s.batMs = 6000;
        auto r = Run(s, ms);
        assert(r.result == Ps2AuxReset::Timeout);
        Ps2AuxDisable(s);
        assert(!s.auxEnabled);
        assert((s.cfg & 0x02) == 0 && (s.cfg & 0x20) != 0);
        s.us += 3000 * 1000;            // past the late BAT
        s.Queue(0x1C, false, s.us);     // a key press
        const uint8_t status = s.In(0x64);
        assert((status & 1) && !(status & 0x20));
        assert(s.In(0x60) == 0x1C);
    }
    {   // Stuck clock, no device: the iteration cap still terminates.
        Sim8042 s; s.device = false; s.clockStuck = true;
        auto r = Run(s, ms);
        assert(r.result == Ps2AuxReset::NoDevice);
    }
    std::puts("test_ps2_aux_reset: ok");
    return 0;
}
