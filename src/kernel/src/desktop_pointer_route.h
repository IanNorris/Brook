#pragma once
#include <stdint.h>
namespace brook {
enum class DesktopPointerEvent { Move, Down, Up, Scroll };
struct DesktopTitleClicks {
    uint64_t identity=0,tick=0;int32_t x=0,y=0;unsigned slop=0;bool armed=false;
    void Reset(){armed=false;}
    void Motion(int32_t nextX,int32_t nextY){
        int64_t dx=static_cast<int64_t>(nextX)-x,dy=static_cast<int64_t>(nextY)-y;
        if(dx>slop||dx<-static_cast<int64_t>(slop)||dy>slop||dy<-static_cast<int64_t>(slop))Reset();
    }
    bool Press(uint64_t window,int32_t nextX,int32_t nextY,uint64_t now,unsigned tolerance){
        Motion(nextX,nextY);
        const bool twice=armed&&identity==window&&window&&now>=tick&&now-tick<=400;
        identity=window;tick=now;x=nextX;y=nextY;slop=tolerance;armed=!twice;
        return twice;
    }
};
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
