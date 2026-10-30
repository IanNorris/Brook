#pragma once
#include <stdint.h>
namespace brook {
enum class XhciIntelRouteResult { NotApplicable, Unavailable, NotLatched, Ready };
struct XhciIntelRouteState {
    uint32_t usb2Before=0,usb2Mask=0,ssBefore=0,ssMask=0,usb2After=0,ssAfter=0;
};
// Intel 9-series PCH datasheet 330550-002, sections 16.1.36--39.
// Firmware's masks grant permission: never change a selector outside its mask,
// nor modify the masks themselves or the write-once port-disable overrides.
template<class Io> XhciIntelRouteResult XhciEnableIntelRouting(
        uint16_t vendor,uint16_t device,uint32_t pciClass,Io& io,XhciIntelRouteState& state) {
    if(vendor!=0x8086 || device!=0x9cb1 || pciClass!=0x0c0330)return XhciIntelRouteResult::NotApplicable;
    state.usb2Before=io.read(0xd0);state.usb2Mask=io.read(0xd4);
    state.ssBefore=io.read(0xd8);state.ssMask=io.read(0xdc);
    if(state.usb2Before==~0u || state.usb2Mask==~0u || state.ssBefore==~0u || state.ssMask==~0u)
        return XhciIntelRouteResult::Unavailable;
    const auto usb2Allowed=state.usb2Mask&0x7fffu,ssAllowed=state.ssMask&0x3fu;
    const auto ssTarget=state.ssBefore|ssAllowed;
    const auto usb2Target=state.usb2Before|usb2Allowed;
    // Enable SuperSpeed first, avoiding an unnecessary USB2 fallback connect.
    if(ssTarget!=state.ssBefore)io.write(0xd8,ssTarget);
    state.ssAfter=io.read(0xd8);
    if(state.ssAfter==~0u)return XhciIntelRouteResult::Unavailable;
    if((state.ssAfter&ssAllowed)!=ssAllowed) {
        state.usb2After=state.usb2Before;return XhciIntelRouteResult::NotLatched;
    }
    if(usb2Target!=state.usb2Before)io.write(0xd0,usb2Target);
    state.usb2After=io.read(0xd0);
    if(state.usb2After==~0u)return XhciIntelRouteResult::Unavailable;
    return (state.usb2After&usb2Allowed)==usb2Allowed && (state.ssAfter&ssAllowed)==ssAllowed
        ?XhciIntelRouteResult::Ready:XhciIntelRouteResult::NotLatched;
}
}
