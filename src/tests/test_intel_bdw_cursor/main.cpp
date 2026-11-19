#include "../../kernel/src/intel_bdw_cursor.h"

#include <cassert>
#include <vector>

using namespace brook;

struct Mmio {
    std::vector<uint32_t> writes;
    uint32_t control = 0, base = 0, position = 0, live = 0, liveControl = 0;
    bool dropEnable = false, dropDisable = false;
    void write32(uint32_t reg, uint32_t value) {
        writes.push_back(reg);
        if (reg == BdwCursorRegs::Control && !(value ? dropEnable : dropDisable))control = value;
        if (reg == BdwCursorRegs::Base)base = value;
        if (reg == BdwCursorRegs::Position)position = value;
    }
    uint32_t read32(uint32_t reg) const {
        if (reg == BdwCursorRegs::Control)return control;
        if (reg == BdwCursorRegs::Base)return base;
        if (reg == BdwCursorRegs::Live)return live;
        return position;
    }
    void vblank() {
        liveControl = control;
        // Leave the last surface address visible even after disabling.
        if (liveControl)live = base | 1;
    }
};

int main()
{
    uint32_t position = 0;
    assert(BdwCursorPosition(-4, -9, position) && position == 0x80098004);
    assert(BdwCursorPosition(3199, 1799, position) && position == 0x07070c7f);
    assert(!BdwCursorPosition(INT32_MIN, 0, position));

    Mmio regs;
    BdwCursorPlane plane;
    assert(!plane.update(regs, 0x2001, 10, 0, 0) && regs.writes.empty());
    assert(plane.update(regs, 0x2000, 10, -4, -9));
    assert((regs.writes == std::vector<uint32_t>{0x700a0, 0x70080, 0x70088, 0x70084}));
    assert(plane.enabled() && plane.blocksSoftware() && !plane.active() && !plane.ready());
    assert(plane.poll(regs, 10));
    assert(!plane.update(regs, 0x6000, 10, 1, 2));
    regs.vblank();
    assert(plane.poll(regs, 11) && plane.active() && plane.ready());
    regs.writes.clear();
    assert(plane.update(regs, 0x2000, 11, 1, 2));
    assert((regs.writes == std::vector<uint32_t>{0x70088, 0x70084}));

    BdwCursorHandoff handoff;
    const uint64_t first=handoff.offer(70);
    handoff.presented(first,70);
    assert(!handoff.mayEnable(70));
    handoff.presented(first,72);
    assert(!handoff.mayEnable(71) && handoff.mayEnable(72));
    assert(handoff.offer(75)==first && handoff.mayEnable(72));

    // Alternate image buffers at every vblank. Each one is kept out of the
    // software layer even while the next hardware image is still pending.
    uint32_t fallbacks=0;
    uint32_t pixels[64*64]{};
    assert(!BdwCursorHasAlpha(pixels,1,1));
    pixels[0]=0x00ffffff;
    assert(!BdwCursorHasAlpha(pixels,1,1));
    for(uint32_t frame=12;frame<1012;++frame){
        pixels[0]=(frame&1)?0xff010203:0x00010203;
        assert(BdwCursorHasAlpha(pixels,1,1)==bool(frame&1));
        const bool rendererHasCursor=!(handoff.epoch() || plane.blocksSoftware());
        if(!plane.update(regs,(frame&1)?0x2000:0x6000,frame,0,0)){
            ++fallbacks;plane.disable(regs,frame);
        }
        assert(plane.active() && !plane.ready());
        assert(plane.poll(regs,frame));
        assert(!rendererHasCursor && regs.liveControl);
        regs.vblank();
        if(!plane.poll(regs,frame+1)){
            ++fallbacks;plane.disable(regs,frame+1);
        }
        assert(plane.active() && plane.ready() && plane.blocksSoftware());
    }
    assert(fallbacks==0);

    // A disable write does not allow composition to expose its cursor yet.
    handoff.cancel();
    plane.disable(regs,1012);
    assert(!plane.active() && plane.blocksSoftware() && regs.liveControl && !regs.control);
    assert(plane.poll(regs,1012) && plane.blocksSoftware());
    regs.vblank();
    assert(plane.poll(regs,1013) && plane.blocksSoftware());
    regs.vblank();
    assert(plane.poll(regs,1014) && !plane.blocksSoftware() && !regs.liveControl);
    const auto second=handoff.offer(0);
    assert(second!=first);
    handoff.presented(first,73);
    assert(!handoff.mayEnable(73));

    // A cursor enable or disable that cannot be read back never exposes both.
    regs.dropEnable=true;
    assert(!plane.update(regs,0x6000,1014,0,0));
    assert(plane.blocksSoftware());
    plane.disable(regs,1014);
    assert(plane.poll(regs,1016) && !plane.blocksSoftware());
    regs.dropEnable=false;
    assert(plane.update(regs,0x6000,1016,0,0));
    regs.vblank();
    assert(plane.poll(regs,1017));
    regs.dropDisable=true;
    plane.disable(regs,1017);
    assert(!plane.poll(regs,1019) && plane.blocksSoftware());
    regs.dropDisable=false;
    assert(!plane.poll(regs,1021) && plane.blocksSoftware());
    regs.vblank();
    assert(plane.poll(regs,1023) && !plane.blocksSoftware());
}
