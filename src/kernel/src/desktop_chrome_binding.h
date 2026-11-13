#pragma once
#include "window.h"
namespace brook {
// Chrome is paired with the retained client's identity, backing and captured
// geometry. Visual state such as title, focus and pointer hover may be newer.
inline bool DesktopChromeBind(const WmDesktopLayers& scene,const WmDesktopLayer& observed,uint32_t& mask) {
    if(scene.count>WM_MAX_WINDOWS)return false;
    for(uint32_t i=0;i<scene.count;++i){
        const auto& old=scene.layers[i];
        if(old.window!=observed.window || old.incarnation!=observed.incarnation)continue;
        if(old.backing!=observed.backing || old.width!=observed.width || old.height!=observed.height ||
           old.stride!=observed.stride || old.bytes!=observed.bytes || old.clientWidth!=observed.clientWidth ||
           old.clientHeight!=observed.clientHeight || old.clientX!=observed.clientX || old.clientY!=observed.clientY ||
           old.upscale!=observed.upscale || old.noChrome!=observed.noChrome)return false;
        mask|=uint32_t{1}<<i;return true;
    }
    return false;
}
inline bool DesktopChromeComplete(uint32_t count,uint32_t mask) {
    if(count>WM_MAX_WINDOWS)return false;
    const uint32_t required=count==32?~uint32_t{0}:(uint32_t{1}<<count)-1;
    return (mask&required)==required;
}
// Carry only surfaces whose captured window identity, backing and geometry
// still match. New or moved windows need a fresh capture before presentation.
inline uint32_t DesktopChromeCarry(const WmDesktopLayers& oldScene,uint32_t oldMask,const WmDesktopLayers& newScene) {
    if(oldScene.count>WM_MAX_WINDOWS || newScene.count>WM_MAX_WINDOWS)return 0;
    uint32_t carried=0;
    for(uint32_t i=0;i<oldScene.count;++i)if(oldMask&(uint32_t{1}<<i))
        (void)DesktopChromeBind(newScene,oldScene.layers[i],carried);
    return carried;
}
}
