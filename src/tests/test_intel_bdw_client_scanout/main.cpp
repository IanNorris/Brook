#undef NDEBUG
#include <cassert>
#include <cstdint>
#include "intel_bdw_client_scanout.h"

using namespace brook;

struct FakeDisplay {
    uint32_t frame=0, live=0, base=0, stride=12800, offset=0;
    uint32_t control=BdwFlipRegs::GopControl;
    uint32_t order[4]{};
    uint32_t writes=0;

    uint32_t read32(uint32_t reg) const {
        switch(reg){
        case BdwFlipRegs::FrameCountA:return frame;
        case BdwFlipRegs::SurfaceLive:return live;
        case BdwFlipRegs::Surface:return base;
        case BdwFlipRegs::Stride:return stride;
        case BdwFlipRegs::TileOffset:return offset;
        case BdwFlipRegs::PlaneControl:return control;
        default:assert(false);return 0;
        }
    }
    void write32(uint32_t reg,uint32_t value){
        order[writes++%4]=reg;
        switch(reg){
        case BdwFlipRegs::Surface:base=value;break;
        case BdwFlipRegs::Stride:stride=value;break;
        case BdwFlipRegs::TileOffset:offset=value;break;
        case BdwFlipRegs::PlaneControl:control=value;break;
        default:assert(false);
        }
    }
    void Vblank(){++frame;live=base;}
};

int main(){
    constexpr BdwClientScanoutLayout linear{12800,false}, tiled{13312,true};
    assert(linear.Valid() && tiled.Valid() && tiled.Pages()>linear.Pages());
    assert((!BdwClientScanoutLayout{12864,true}.Valid()));
    assert((!BdwClientScanoutLayout{16385,false}.Valid()));
    constexpr BdwClientScanoutImage a{0x10000000,1,linear}, b{0x12000000,2,tiled};
    FakeDisplay io;
    BdwClientScanoutFlip flip;
    assert(flip.Arm(io,a,0));
    assert(io.order[0]==BdwFlipRegs::Stride && io.order[1]==BdwFlipRegs::TileOffset &&
           io.order[2]==BdwFlipRegs::PlaneControl && io.order[3]==BdwFlipRegs::Surface);
    assert(flip.Contains(a.base) && !flip.Poll(io) && !flip.Arm(io,b,0));
    io.Vblank();
    assert(flip.Poll(io) && flip.Front().identity==a.identity && !flip.Armed().identity);
    assert(flip.Arm(io,b,0) && io.stride==tiled.stride && io.control==tiled.Control());
    assert(flip.Contains(a.base) && flip.Contains(b.base));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Contains(a.base) && flip.Front().identity==b.identity);
    assert(flip.Arm(io,a,0));
    assert(flip.Restore(io,0));
    assert(flip.Contains(b.base) && flip.Contains(a.base) && !flip.Poll(io));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Contains(b.base) && !flip.Contains(a.base));
    assert(io.stride==12800 && io.control==BdwFlipRegs::GopControl && !io.offset);

    // A fallback to the last kernel-owned image also retains a pending
    // client until that image latches, even if the live register was equal.
    constexpr uint32_t kernelFront=0x14000000;
    io.base=io.live=kernelFront;
    assert(flip.Arm(io,a,kernelFront));
    assert(flip.Restore(io,kernelFront) && !flip.Poll(io));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Contains(a.base));
    assert(flip.Arm(io,b,kernelFront));
    io.frame+=4;
    assert(!flip.Poll(io) && flip.Uncertain() && flip.Contains(b.base));
    assert(flip.Restore(io,0) && !flip.Poll(io));
    io.Vblank();
    assert(flip.Poll(io) && !flip.Uncertain());
    assert(flip.Arm(io,a,0));
    io.Vblank();io.stride=13312;
    assert(!flip.Poll(io) && flip.Contains(a.base));
}
