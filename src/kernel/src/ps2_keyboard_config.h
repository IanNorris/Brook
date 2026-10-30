#pragma once
#include <stdint.h>
namespace brook {
struct Ps2KeyboardConfig { uint8_t before=0, requested=0, after=0; unsigned stage=0; };
template<class Io> bool ConfigurePs2Keyboard(Io& io, Ps2KeyboardConfig& result) {
    auto command=[&](uint8_t c) { if(!io.waitWrite()) return false; io.command(c); return true; };
    result.stage=1;
    // Quiesce port 1 so command-byte replies cannot be mistaken for keystrokes.
    if(!command(0xAD) || !io.waitWrite()) return false;
    io.drain();
    result.stage=2;
    if(!command(0x20) || !io.readController(result.before)) return false;
    result.requested=(result.before | 0x41) & ~0x10;
    result.stage=3;
    if(!command(0x60) || !io.waitWrite()) return false;
    io.data(result.requested | 0x10); // keep port quiesced until readback
    result.stage=4;
    if(!command(0x20) || !io.readController(result.after)) return false;
    if(result.after != uint8_t(result.requested | 0x10)) return false;
    result.stage=5;
    if(!command(0xAE) || !io.waitWrite()) return false;
    result.stage=0; return true;
}
}
