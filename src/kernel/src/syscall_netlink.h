#pragma once

// syscall_netlink.h -- internal declarations shared from syscall_netlink.inc (compiled in syscall_socket.cpp).
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "socket_identity.h"
#include "user_copy.h"
#include "syscall_internal.h"

namespace brook {

#include "syscall_netlink_types.inc"
void NetlinkRef(NetlinkSocket* s);
void NetlinkUnref(NetlinkSocket* s);
uint32_t NetlinkReady(NetlinkSocket* s,uint32_t events);

} // namespace brook
