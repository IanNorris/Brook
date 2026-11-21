#pragma once

#include "resize_rpc.h"
#include "../../kernel/src/vmware_platform.h"

namespace brook::svga {

// Low-bandwidth VMware MESSAGE adapter. There is no probe on physical PCs or
// other hypervisors, no general command interface, and no host buffer address.
class ResizeRpcPort {
    bool available_;

public:
    ResizeRpcPort() : available_(VmwarePlatformPresent(
        [](uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(leaf), "c"(0));
        })) {}

    bool operator()(resize_rpc::Registers& r) const
    {
        using namespace resize_rpc;
        const uint16_t op = High(r.cx);
        if (!available_ || r.ax != Magic || uint16_t(r.dx) != Port ||
            uint16_t(r.cx) != MessageCommand || op > uint16_t(Operation::Close)) return false;
        if (op == uint16_t(Operation::Open)) {
            const uint32_t protocol = r.bx & ~CookieFlag;
            if (protocol != Tclo && protocol != Rpci) return false;
        }
        __asm__ volatile("inl %%dx,%%eax"
                         : "+a"(r.ax), "+b"(r.bx), "+c"(r.cx), "+d"(r.dx), "+S"(r.si), "+D"(r.di)
                         : : "cc", "memory");
        return true;
    }
};

}
