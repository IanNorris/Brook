#include "../../kernel/src/intel_bdw_cursor.h"
#include "../../shared/brook_debug_frame.h"

#include <cassert>
#include <vector>

using namespace brook;

static_assert(int(BROOK_DESKTOP_OVERLAY_SURFACES) != int(BROOK_DESKTOP_OVERLAY_CURSOR_PRESENTED));

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

static void enableWithBuiltInArrow(bool move)
{
    Mmio regs;
    BdwCursorPlane plane;
    BdwCursorHandoff handoff;
    const int32_t x=120,y=80;
    assert(handoff.prepare(20) && handoff.probing());
    assert(!handoff.offer(7,20,false) && !handoff.suppress(false));
    // The initial surface is transparent: keep compositing the arrow until
    // hardware proves it can latch. No position or image event is needed.
    assert(plane.update(regs,0x2000,1,0,0));
    assert(!handoff.offer(7,21,plane.active()) && !handoff.suppress(false));
    regs.vblank();
    assert(plane.poll(regs,2) && plane.active() && !handoff.owned());
    const uint64_t epoch=handoff.offer(7,37,plane.active());
    assert(epoch && handoff.suppress(false) && handoff.awaitingPresent());
    assert(!handoff.presented(epoch,7) && !handoff.mayEnable(8));
    assert(handoff.presented(epoch,8));
    assert(!handoff.mayEnable(7) && handoff.mayEnable(8));
    // The primary is now clear. Arm the unchanged built-in arrow.
    handoff.arm(52);
    assert(plane.update(regs,0x6000,3,x,y) && !plane.ready() && !handoff.owned());
    regs.vblank();
    assert(plane.poll(regs,4) && plane.ready() && plane.active());
    handoff.latched();
    assert(handoff.owned() && handoff.suppress(true));
    uint32_t expected=0;
    assert(BdwCursorPosition(x,y,expected) && plane.position()==expected);
    if(move){
        regs.writes.clear();
        assert(plane.update(regs,0x6000,4,x+2,y+1));
        assert((regs.writes==std::vector<uint32_t>{BdwCursorRegs::Position,BdwCursorRegs::Base}));
        assert(BdwCursorPosition(x+2,y+1,expected) && plane.position()==expected);
    }
}

static void startupTimeouts()
{
    Mmio regs;
    BdwCursorPlane plane;
    BdwCursorHandoff handoff;
    assert(handoff.prepare(100) && !handoff.suppress(false));
    assert(plane.update(regs,0x2000,1,0,0));
    assert(!handoff.offer(10,349,false) && !handoff.expired(349));
    assert(handoff.expired(350) && !handoff.suppress(false));
    handoff.cancel();
    plane.disable(regs,1);
    assert(!handoff.suppress(false)); // probe never had visible pixels
    assert(plane.poll(regs,3) && !plane.blocksSoftware());

    assert(handoff.prepare(400) && plane.update(regs,0x2000,5,0,0));
    regs.vblank();
    assert(plane.poll(regs,6));
    assert(handoff.offer(10,420,plane.active()) && handoff.awaitingPresent());
    assert(!handoff.expired(669) && handoff.expired(670)); // missing clean-frame ack
    handoff.cancel();
    plane.disable(regs,6);
    assert(!handoff.suppress(false));
}

int main()
{
    enableWithBuiltInArrow(false);
    enableWithBuiltInArrow(true);
    startupTimeouts();
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
    assert(handoff.prepare(0));
    const uint64_t first=handoff.offer(70,0,true);
    handoff.presented(first,70);
    assert(!handoff.mayEnable(70));
    handoff.presented(first,72);
    assert(!handoff.mayEnable(71) && handoff.mayEnable(72));
    assert(handoff.offer(75,0,true)==first && handoff.mayEnable(72));
    handoff.arm(0);
    handoff.latched();

    // Alternate image buffers at every vblank. Each one is kept out of the
    // software layer even while the next hardware image is still pending.
    uint32_t fallbacks=0;
    uint32_t pixels[64*64]{};
    uint32_t wide[128*2]{}, packed[64*2]{};
    wide[128+1]=packed[64+1]=0xff112233;
    assert(BdwCursorImageKey(wide,2,2,128)==BdwCursorImageKey(packed,2,2));
    assert(!BdwCursorImageKey(pixels,1,1));
    pixels[0]=0x00ffffff;
    assert(!BdwCursorImageKey(pixels,1,1));
    BdwCursorImages images;
    unsigned uploads=0;
    for(uint32_t frame=12;frame<1012;++frame){
        pixels[0]=(frame&1)?0xff010203:0x00010203;
        assert(bool(BdwCursorImageKey(pixels,1,1))==bool(frame&1));
        const bool rendererHasCursor=!handoff.suppress(plane.blocksSoftware());
        const auto key=BdwCursorImageKey(pixels,1,1);
        assert(key==BdwCursorImageKey(pixels,2,2));
        const auto slot=images.select(key,false);
        uploads+=images.needsUpload(slot,key);
        images.selectProgrammed(slot,key);
        assert(images.transparent()==!key);
        assert(images.select(key,false)==slot && !images.needsUpload(slot,key));
        if(!plane.update(regs,slot?0x6000:0x2000,frame,0,0)){
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
    assert(fallbacks==0 && uploads==2);

    // A disable write does not allow composition to expose its cursor yet.
    handoff.cancel();
    plane.disable(regs,1012);
    assert(!plane.active() && plane.blocksSoftware() && regs.liveControl && !regs.control);
    assert(plane.poll(regs,1012) && plane.blocksSoftware());
    regs.vblank();
    assert(plane.poll(regs,1013) && plane.blocksSoftware());
    regs.vblank();
    assert(plane.poll(regs,1014) && !plane.blocksSoftware() && !regs.liveControl);
    assert(handoff.prepare(1));
    const auto second=handoff.offer(0,1,true);
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
