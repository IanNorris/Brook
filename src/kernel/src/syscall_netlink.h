#pragma once

// syscall_netlink.h -- internal declarations shared from syscall_netlink.inc (compiled in syscall_socket.cpp).
// Internal to the syscall layer; split from syscall.cpp.

#include "socket_identity.h"
#include "user_copy.h"
#include "syscall_internal.h"

namespace brook {

#include "syscall_netlink_types.inc"
void NetlinkRef(NetlinkSocket* s);
void NetlinkUnref(NetlinkSocket* s);
uint32_t NetlinkReady(NetlinkSocket* s,uint32_t events);
uint32_t NetlinkAvailable(NetlinkSocket* s);

int64_t NetlinkSend(NetlinkSocket* s,uint64_t address,uint64_t length,uint64_t flags,
                    uint64_t destination,uint64_t destinationLength,bool user);

int64_t NetlinkVectorIo(NetlinkSocket* s,uint64_t address,uint64_t count,bool writing);

int64_t NetlinkReadUser(NetlinkSocket* s,uint64_t address,uint64_t length,uint64_t flags,
                       uint64_t source=0,uint64_t sourceLength=0);

int NetlinkInterfaceIoctl(uint64_t command,uint64_t address);

} // namespace brook
