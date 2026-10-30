#include "../../kernel/src/ps2_keyboard_config.h"
#include <cassert>
#include <vector>
struct Controller {
    uint8_t config; bool pending=false, writing=false;
    unsigned waits=0, failWait=0, reads=0, failRead=0;
    bool corrupt=false;
    std::vector<unsigned> commands;
    bool waitWrite() { return ++waits!=failWait; }
    void command(uint8_t c) {
        commands.push_back(c);
        switch(c) {
            case 0xAD:config|=0x10;break;
            case 0xAE:config&=~0x10;break;
            case 0x20:pending=true;break;
            case 0x60:writing=true;break;
            default:assert(false);
        }
    }
    void data(uint8_t b) { assert(writing); writing=false; config=b; }
    void drain() {}
    bool readController(uint8_t& b) {
        assert(pending);pending=false;
        if(++reads==failRead) return false;
        b=config;
        if(corrupt && reads==2) b^=0x40;
        return true;
    }
};
int main() {
    for(unsigned old=0;old<256;++old) {
        Controller io{static_cast<uint8_t>(old),false,false,0,0,0,0,false,{}};
        brook::Ps2KeyboardConfig result;
        assert(brook::ConfigurePs2Keyboard(io,result));
        assert(result.stage==0 && result.after==((old|0x41)|0x10));
        assert((io.config&0xAE)==(old&0xAE));
        assert((io.commands==std::vector<unsigned>{0xAD,0x20,0x60,0x20,0xAE}));
    }
    for(unsigned failure=1;failure<=8;++failure) {
        Controller io{0,false,false,0,failure,0,0,false,{}};
        brook::Ps2KeyboardConfig result;
        assert(!brook::ConfigurePs2Keyboard(io,result));assert(result.stage);
    }
    for(unsigned failure=1;failure<=2;++failure) {
        Controller io{0,false,false,0,0,0,failure,false,{}};
        brook::Ps2KeyboardConfig result;
        assert(!brook::ConfigurePs2Keyboard(io,result));assert(result.stage);
    }
    Controller io{0,false,false,0,0,0,0,true,{}};
    brook::Ps2KeyboardConfig result;
    assert(!brook::ConfigurePs2Keyboard(io,result));assert(result.stage==4);
}
