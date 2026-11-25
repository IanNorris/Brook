#pragma once

#include "device.h"

namespace brook {
// Serialized by the MSC I/O lock, including all byte-to-block writes.
struct UsbWriteWindows {
    DeviceWriteWindow update{}, system{};

    static bool Contains(const DeviceWriteWindow& window,uint64_t start,uint64_t length) {
        return window.length && length && start>=window.start && length<=window.length &&
            start-window.start<=window.length-length;
    }
    bool Allows(uint64_t start,uint64_t length) const {
        return Contains(update,start,length) || Contains(system,start,length);
    }
    bool Set(bool persistent,DeviceWriteWindow requested,uint64_t bytes,uint32_t block) {
        auto& selected=persistent?system:update;
        const auto& other=persistent?update:system;
        if(requested.length) {
            if(!block || requested.start%block || requested.length%block ||
               requested.start>bytes || requested.length>bytes-requested.start)return false;
            if(other.length && requested.start<other.start+other.length &&
               other.start<requested.start+requested.length)return false;
            if(selected.length && (selected.start!=requested.start || selected.length!=requested.length))
                return false;
        }
        selected=requested.length?requested:DeviceWriteWindow{};
        return true;
    }
};
}
