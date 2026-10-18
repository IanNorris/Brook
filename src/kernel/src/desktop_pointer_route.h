#pragma once
#include <stdint.h>
namespace brook {
enum class DesktopPointerEvent { Move, Down, Up, Scroll };
struct DesktopPointerGate {
    uint32_t ownedButtons=0;
    bool consume(DesktopPointerEvent event,unsigned button,bool overlay) {
        const uint32_t bit=button<32?uint32_t(1)<<button:0;
        if(event==DesktopPointerEvent::Down) {
            if(overlay)ownedButtons|=bit;
            else ownedButtons&=~bit;
            return overlay;
        }
        if(event==DesktopPointerEvent::Up) {
            const bool owned=(ownedButtons&bit)!=0;
            ownedButtons&=~bit;
            return overlay || owned;
        }
        return overlay;
    }
};
}
