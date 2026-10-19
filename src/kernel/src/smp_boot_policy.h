#pragma once
#include <stdint.h>

namespace brook {
// Logical CPU indices must form a dense prefix: existing scheduler/TLB users
// iterate [0, onlineCount). Normalize before any AP caches its logical index.
inline uint32_t SmpOrderApicIds(const uint8_t* input, uint32_t count,
                               uint8_t bsp, uint8_t* output, uint32_t capacity) {
    if (!capacity || !output || (count && !input)) return 0;
    output[0]=bsp;
    uint32_t used=1;
    for (uint32_t i=0;i<count;++i) {
        bool duplicate=false;
        for (uint32_t j=0;j<used;++j) if (output[j]==input[i]) duplicate=true;
        if (duplicate) continue;
        if (used==capacity) return 0;
        output[used++]=input[i];
    }
    return used;
}
class SmpBootAdmission {
    uint32_t online_=1;
    bool stopped_=false;
public:
    uint32_t Count() const { return online_; }
    bool CanStart(uint32_t slot) const { return !stopped_ && slot==online_; }
    bool Accept(uint32_t slot,uint32_t acknowledgement) {
        if (!CanStart(slot) || acknowledgement!=slot+1) return false;
        ++online_;return true;
    }
    void Stop() { stopped_=true; }
};
}
