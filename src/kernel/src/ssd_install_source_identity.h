#pragma once
#include "device.h"
#include "gpt.h"

namespace brook {
// The USB discovery code registers usb0..usb3 and their partition children.
// Require pointer identity and scanner-owned parentage; a device name on the
// leased file alone cannot prove that it is separate from the install target.
inline bool SsdInstallUsbSource(Device* source,Device* target) {
    if(!source || !target || source==target || !DeviceIsRegistered(source))return false;
    Device* parent=PartitionParentDevice(source);
    if(parent==target)return false;
    for(unsigned i=0;i<4;++i) {
        char name[]="usb0";name[3]=char('0'+i);
        Device* usb=DeviceFind(name);
        if(usb && usb!=target && (source==usb || parent==usb))return true;
    }
    return false;
}
}
