#include "../../kernel/src/intel_bdw_cursor.h"

#include <cassert>
#include <vector>

using namespace brook;

struct Mmio {
    std::vector<uint32_t> writes;
    uint32_t control = 0, base = 0, position = 0, live = 0;
    bool dropEnable = false;
    void write32(uint32_t reg, uint32_t value) {
        writes.push_back(reg);
        if (reg == BdwCursorRegs::Control && (!value || !dropEnable))control = value;
        if (reg == BdwCursorRegs::Base)base = value;
        if (reg == BdwCursorRegs::Position)position = value;
    }
    uint32_t read32(uint32_t reg) const {
        if (reg == BdwCursorRegs::Control)return control;
        if (reg == BdwCursorRegs::Base)return base;
        if (reg == BdwCursorRegs::Live)return live;
        return position;
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
    assert(plane.enabled() && plane.base() == 0x2000 && regs.control == 0x27);
    assert(!plane.canUpload(11, regs.live));
    regs.writes.clear();
    assert(plane.update(regs, 0x2000, 11, 1, 2));
    assert((regs.writes == std::vector<uint32_t>{0x70088, 0x70084}));
    assert(!plane.update(regs, 0x6000, 11, 1, 2));
    assert(!plane.update(regs, 0x6000, 12, 1, 2));
    regs.live = 0x2000;
    assert(plane.update(regs, 0x6000, 12, 1, 2));
    assert(plane.disable(regs, 12) && !plane.enabled() && !regs.base && !regs.control);
    assert(!plane.canUpload(13, 0) && plane.canUpload(14, 0));
    regs.live = 0;

    regs.dropEnable = true;
    assert(!plane.update(regs, 0x6000, 14, 0, 0));
    assert(!plane.enabled() && !regs.base && !regs.control);
}
