// syscall_socket.cpp -- socket syscalls (inet, AF_UNIX and netlink), SCM_RIGHTS and msghdr I/O.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "tcp_info.h"
#include "process_user_namespace.h"

#include "syscall.h"
#include "signal_fd.h"
#include "user_access_scope.h"
#include "intel_bdw_buffer.h"
#include "compositor_scene.h"
#include "net.h"
#include "syscall_internal.h"
#include "syscall_eventfd.h"
#include "syscall_timerfd.h"
#include "syscall_memfd.h"
#include "syscall_unix_socket.h"
#include "syscall_drm.h"
#include "syscall_poll.h"
#include "syscall_epoll.h"
#include "syscall_fd.h"
#include "syscall_open.h"
#include "syscall_rw.h"
#include "syscall_netlink.h"
#include "syscall_socket.h"
#include "syscall_handlers.h"

namespace brook {

#include "syscall_netlink.inc"

// ---------------------------------------------------------------------------
// Socket syscalls
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Per-process socket-to-fd mapping.
// Socket index is stored in FdEntry::handle as (void*)(uintptr_t)(sockIdx + 1).
// +1 so that socket 0 maps to non-null handle.

int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;
    KernelOperationGuard operation(proc);

    DbgPrintf("sys_socket: domain=%lu type=%lu proto=%lu\n", domain, type, protocol);

    if(domain==16) {
        if(protocol!=0)return -93;
        if((type&~uint64_t(0x800|0x80000|0xf)) || ((type&0xf)!=2 && (type&0xf)!=3))return -EINVAL;
        auto* socket=static_cast<NetlinkSocket*>(kmalloc(sizeof(NetlinkSocket)));
        if(!socket)return -ENOMEM;
        *socket=NetlinkSocket{};
        const auto actor=ProcessFileActorAcquire(proc,nullptr);
        socket->identity=NetAllocateSocketIdentity(actor.uid,actor.gid);ProcessGroupsRelease(actor.groups);
        if(!socket->identity.inode){kfree(socket);return -ENOMEM;}
        socket->type=type&0xf;socket->nonblock=(type&0x800)!=0;
        SpinLockAcquire(&g_netlinkRegistryLock);socket->next=g_netlinkSockets;g_netlinkSockets=socket;SpinLockRelease(&g_netlinkRegistryLock);
        FdEntry entry{};entry.type=FdType::NetlinkSocket;entry.handle=socket;entry.fdFlags=(type&0x80000)?1:0;
        SocketIdentityLabel(socket->identity.inode,entry.dirPath,sizeof(entry.dirPath));
        int result=FdPublishNew(proc,entry,2|(type&0x800));
        if(result<0){NetlinkUnref(socket);return -EMFILE;}return result;
    }
    if (domain == AF_UNIX) {
        auto* usd = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
        if (!usd) return -ENOMEM;
        UnixLife(UnixLifeEvent::Allocate, usd, 0, 1, -1,
                 reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
        memset(usd, 0, sizeof(UnixSocketData));
        usd->state    = UnixSocketData::State::Unbound;
        usd->nonblock = (type & UNIX_SOCK_NONBLOCK) != 0;
        usd->refCount = 1;
        usd->socketType = static_cast<uint32_t>(type) & 0xfu;
        usd->peerCredentials = {0, ~uint32_t{0}, ~uint32_t{0}};

        int fd = UnixInstallSocket(proc, usd, static_cast<uint32_t>(type), "socket");
        if (fd < 0) { UnixSocketHandleUnref(usd); return fd; }
        DbgPrintf("sys_socket: AF_UNIX fd=%d\n", fd);
        return fd;
    }

    // AF_INET6 (10) — not supported
    if (domain != AF_INET) {
        // Only log interesting rejections (AF_INET6), not AF_NETLINK spam
        if (domain == 10) { // AF_INET6
        }
        return -EAFNOSUPPORT;
    }

    int sockIdx = SockCreate(static_cast<int>(domain),
                              static_cast<int>(type & 0xFF), // mask SOCK_NONBLOCK etc
                              static_cast<int>(protocol));
    if (sockIdx < 0) return -ENOMEM;

    // Record nonblocking flag
    if (type & 0x800) // SOCK_NONBLOCK
        brook::SockSetNonblock(sockIdx, true);

    // Allocate an fd for this socket
    FdEntry owned{};
    owned.type = FdType::Socket;
    owned.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(sockIdx + 1));
    owned.fdFlags = (type & UNIX_SOCK_CLOEXEC) ? 1 : 0;
    brook::SocketIdentity identity{};
    if (!brook::SockGetIdentity(sockIdx, &identity) ||
        !brook::SocketIdentityLabel(identity.inode, owned.dirPath, sizeof(owned.dirPath))) {
        SockClose(sockIdx); return -ENOMEM;
    }
    int fd = FdPublishNew(proc,owned,type & UNIX_SOCK_NONBLOCK);
    if (fd < 0) {
        SockClose(sockIdx);
        return -EMFILE;
    }

    DbgPrintf("sys_socket: fd=%d sockIdx=%d\n", fd, sockIdx);
    return fd;
}

static int GetSockIdx(Process* proc, int fd)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return -1;
    FdEntry* e = &proc->fds[fd];
    if (e->type != FdType::Socket) return -1;
    return static_cast<int>(reinterpret_cast<uintptr_t>(e->handle)) - 1;
}

int64_t sys_bind(uint64_t fdVal, uint64_t addrVal, uint64_t addrLen,
                  uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // Check for AF_UNIX socket first
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (addrLen<2 || addrLen>sizeof(SockAddrUn)) return -EINVAL;
        SockAddrUn address{};
        if (!CopyFromUser(&address,addrVal,addrLen)) return -EFAULT;
        if (address.sun_family != AF_UNIX) return -EINVAL;
        UnixNameGuard nameGuard;
        auto* addressBytes=reinterpret_cast<const uint8_t*>(&address);
        if (addrLen==2) {
            // Linux autobind: a unique abstract name "\0" + five hex digits.
            static uint32_t nextAutobind;
            bool found=false;
            for (uint32_t attempt=0;attempt<0x100000 && !found;++attempt) {
                const uint32_t value=(nextAutobind++)&0xfffff;
                static const char hex[]="0123456789abcdef";
                address.sun_path[0]=0;
                for (int digit=0;digit<5;++digit)address.sun_path[1+digit]=hex[(value>>(16-4*digit))&15];
                found=!UnixAbstractNameInUse(addressBytes,8,usd);
            }
            if (!found) return -98; // EADDRINUSE
            addrLen=8;
        }
        if (UnixAbstractNameInUse(addressBytes,static_cast<uint32_t>(addrLen),usd)) return -98; // EADDRINUSE
        SpinLockAcquire(&g_unixRegistryLock);
        if (usd->localAddressLength || usd->state!=UnixSocketData::State::Unbound) {
            SpinLockRelease(&g_unixRegistryLock);return -EINVAL;
        }
        SpinLockRelease(&g_unixRegistryLock);
        char path[109]{};
        memcpy(path, address.sun_path, addrLen - 2);
        Vnode* node = nullptr;
        VnodeStat identity{};
        if (path[0]) {
            AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
            int error=0;
            node=VfsCreateSocketAt(lookup.fs.root,lookup.start(),path,0777,lookup.fs.mask,&error);
            if (!node) return error;
            if ((error = VfsStat(node, &identity))) { VfsClose(node); return error; }
        }
        SpinLockAcquire(&g_unixRegistryLock);
        usd->boundNode = node;
        usd->boundDev = identity.dev;
        usd->boundIno = identity.ino;
        memcpy(usd->localAddress,&address,addrLen);usd->localAddressLength=static_cast<uint32_t>(addrLen);
        for (int i = 0; i < 107 && path[i]; ++i) usd->path[i] = path[i];
        SpinLockRelease(&g_unixRegistryLock);
        UNIX_LIFE(Bind, usd, 0, 0, fd);
        SerialPrintf("sys_bind: AF_UNIX fd=%d path='%s'\n", fd, usd->path);
        return 0;
    }

    if(fde && fde->type==FdType::NetlinkSocket) {
        if(addrLen<sizeof(RouteAddress))return -EINVAL;
        RouteAddress address{};if(!CopyFromUser(&address,addrVal,sizeof(address)))return -EFAULT;
        if(address.family!=16)return -EINVAL;if(address.groups)return -95;
        return NetlinkBind(static_cast<NetlinkSocket*>(fde->handle),address.port);
    }
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    if (!addrVal || addrLen < sizeof(SockAddrIn)) return -EINVAL;
    SockAddrIn addr{};
    if (!CopyFromUser(&addr, addrVal, sizeof(addr))) return -EFAULT;

    return SockBind(sockIdx, &addr);
}

int64_t sys_connect(uint64_t fdVal, uint64_t addrVal, uint64_t addrLen,
                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX connect: find server, create pipe pair, enqueue, block until accepted
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if(fde && fde->type==FdType::NetlinkSocket) {
        if(int e=NetlinkDestination(addrVal,addrLen,false))return e;
        auto* socket=static_cast<NetlinkSocket*>(fde->handle);if(int e=NetlinkBind(socket,0,true))return e;
        __atomic_store_n(&socket->connected,true,__ATOMIC_RELEASE);return 0;
    }
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        UnixNameGuard nameGuard;
        if (usd->state == UnixSocketData::State::Connected) return -106; // EISCONN
        if (usd->state == UnixSocketData::State::Listening) return -EINVAL;
        if (addrLen <= 2 || addrLen > sizeof(SockAddrUn)) return -EINVAL;
        SockAddrUn address{};
        if (!CopyFromUser(&address, addrVal, addrLen)) return -EFAULT;
        if (address.sun_family != AF_UNIX) return -EINVAL;

        // Copy destination path
        char path[109] = {};
        memcpy(path, address.sun_path, addrLen - 2);

        DbgPrintf("sys_connect: AF_UNIX fd=%d path='%s'\n", fd, path);

        // Resolve and pin the named inode, including parent search and socket
        // write permission. The inode pin makes unlink/rebinding unambiguous.
        struct NodePin {
            Vnode* node = nullptr;
            ~NodePin() { if (node) VfsClose(node); }
        } lookup;
        VnodeStat identity{};
        if (path[0]) {
            AtLookup at;if(int error=at.Init(-100,path))return error;
            int error=0;
            lookup.node=VfsOpenAt(at.fs.root,at.start(),path,VFS_O_METADATA,0,at.fs.mask,&error);
            if (!lookup.node) return error ? error : -ENOENT;
            if ((error = VfsAccess(lookup.node, 2, false))) return error;
            if ((error = VfsStat(lookup.node, &identity))) return error;
            if ((identity.mode & 0170000) != 0140000) return -ECONNREFUSED;
        }
        const auto* addressBytes = reinterpret_cast<const uint8_t*>(&address);
        const bool abstract = UnixAddressIsAbstract(addressBytes, static_cast<uint32_t>(addrLen));
        UnixSocketData* server = UnixFindServer(path, lookup.node ? &identity : nullptr,
            abstract ? addressBytes : nullptr, abstract ? static_cast<uint32_t>(addrLen) : 0);
        if (!server) return -ECONNREFUSED;
        struct ServerReference {
            UnixSocketData* value;
            ~ServerReference() { UnixSocketHandleUnref(value); }
        } serverReference{server};
        if((usd->socketType==5 || server->socketType==5) && usd->socketType!=server->socketType)
            return -91; // EPROTOTYPE: both peers must agree on record framing.
        // The pending slot owns the server endpoints before connect returns.
        // A successful local stream connect queues a connection; it does not
        // wait for accept, and must survive the client closing immediately.
        auto* pipeCS = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
        auto* pipeSC = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
        auto* fdqCS = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
        auto* fdqSC = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
        if (!pipeCS || !pipeSC || !fdqCS || !fdqSC) {
            if (pipeCS) PipeBufferDestroy(pipeCS);
            if (pipeSC) PipeBufferDestroy(pipeSC);
            if (fdqCS) kfree(fdqCS);
            if (fdqSC) kfree(fdqSC);
            return -ENOMEM;
        }
        memset(fdqCS, 0, sizeof(UnixFdQueue));
        memset(fdqSC, 0, sizeof(UnixFdQueue));
        pipeCS->readers = 1; pipeCS->writers = 1;
        pipeSC->readers = 1; pipeSC->writers = 1;
        fdqCS->refCount = 2; // client and pending server endpoint
        fdqSC->refCount = 2;

        SpinLockAcquire(&g_unixRegistryLock);
        int slot = -1;
        for (int i = 0; i < UNIX_ACCEPT_QUEUE; ++i)
            if (!server->pending[i].used) { slot = i; break; }
        int error = usd->state == UnixSocketData::State::Connected ? -106 :
                    usd->state == UnixSocketData::State::Listening ? -EINVAL :
                    slot < 0 ? -EAGAIN : 0;
        if (error) {
            SpinLockRelease(&g_unixRegistryLock);
            PipeBufferDestroy(pipeCS); PipeBufferDestroy(pipeSC);
            kfree(fdqCS); kfree(fdqSC);
            return error;
        }
        usd->peerCredentials = server->peerCredentials;
        usd->peerIncarnation=server->peerIncarnation;usd->peerLaunchId=server->peerLaunchId;
        usd->peerAddressLength=server->localAddressLength;
        memcpy(usd->peerAddress,server->localAddress,sizeof(usd->peerAddress));
        usd->rxPipe = pipeSC;
        usd->txPipe = pipeCS;
        usd->incomingFds = fdqSC;
        usd->peerIncomingFds = fdqCS;
        for (int i = 0; i < 107 && path[i]; ++i) usd->path[i] = path[i];
        usd->state = UnixSocketData::State::Connected;

        UnixPendingConn& pending = server->pending[slot];
        pending.serverRx = pipeCS;
        pending.serverTx = pipeSC;
        pending.serverRxFds = fdqCS;
        pending.serverTxFds = fdqSC;
        pending.clientCredentials = UnixCurrentCredentials(proc);
        pending.clientIncarnation=UnixCurrentIncarnation(proc);pending.clientLaunchId=proc->launchId;
        pending.clientAddressLength=usd->localAddressLength;
        memcpy(pending.clientAddress,usd->localAddress,sizeof(pending.clientAddress));
        pending.used = true;
        ++server->pendingCount;
        if (server->acceptWaiter) {
            Process* waiter = server->acceptWaiter;
            server->acceptWaiter = nullptr;
            WakeProcess(waiter);
        }
        UnixListenWakeEpoll(server);
        SpinLockRelease(&g_unixRegistryLock);

        (void)addrLen;
        return 0;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    // Snapshot the supplied sockaddr before logging or entering the network
    // stack. Copy only the advertised length, including storage-size callers.
    if (addrLen < sizeof(brook::SockAddrIn) || addrLen > 128) return -EINVAL;
    alignas(brook::SockAddrIn) uint8_t address[128]{};
    if (!CopyFromUser(address, addrVal, addrLen)) return -EFAULT;
    const auto* uaddr = reinterpret_cast<const brook::SockAddrIn*>(address);

    DbgPrintf("sys_connect: fd=%d sockIdx=%d type=%d addr=%u.%u.%u.%u:%u\n",
                 fd, sockIdx, brook::SockGetType(sockIdx),
                 (brook::ntohl(uaddr->sin_addr) >> 24) & 0xFF,
                 (brook::ntohl(uaddr->sin_addr) >> 16) & 0xFF,
                 (brook::ntohl(uaddr->sin_addr) >> 8) & 0xFF,
                 brook::ntohl(uaddr->sin_addr) & 0xFF,
                 brook::ntohs(uaddr->sin_port));
    // For UDP, "connect" just sets the default destination
    // For TCP, perform the 3-way handshake
    int connectResult = brook::SockConnect(sockIdx, uaddr);
    {
    }
    return connectResult;
}

int64_t sys_sendto(uint64_t fdVal, uint64_t bufVal, uint64_t lenVal,
                    uint64_t flagsVal, uint64_t destVal, uint64_t addrLenVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (!fde || fde->type == FdType::Path) return -EBADF;
    // Libraries use ENOTSOCK to fall back from send(MSG_NOSIGNAL) to write
    // on valid pipe/eventfd/file descriptors (for example PulseAudio pa_write).
    if(fde->type==FdType::NetlinkSocket)return NetlinkSend(static_cast<NetlinkSocket*>(fde->handle),bufVal,lenVal,flagsVal,destVal,addrLenVal);
    if (fde->type != FdType::UnixSocket && fde->type != FdType::Socket)
        return -ENOTSOCK;
    if (fde && fde->type == FdType::UnixSocket) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if(usd->socketType==5 && (flagsVal&~(0x40ULL|0x80ULL|0x4000ULL)))return -95;
        if(lenVal && !UserBufferReadable(bufVal,lenVal))return -EFAULT;
        return UnixSocketWriteSome(usd,(flagsVal&0x40)||(FdStatus(fde)&0x800),
            reinterpret_cast<const char*>(bufVal),lenVal,nullptr,0,(flagsVal&0x4000)!=0);
    }

    if (!fde->handle) return -EBADF;
    int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;

    (void)flagsVal;

    // For connected TCP sockets, use SockSend (stream send)
    if (brook::SockIsStream(sockIdx))
    {
        const uint32_t bytes = static_cast<uint32_t>(lenVal > 0x7ffff000ULL ? 0x7ffff000ULL : lenVal);
        int ret = brook::SockSendUser(sockIdx, bufVal, bytes);
        return static_cast<int64_t>(ret);
    }

    // UDP path: the destination and payload are copied into kernel memory
    // before the network stack sees them.
    SockAddrIn destination{};
    if (destVal) {
        if (addrLenVal < sizeof(SockAddrIn)) return -EINVAL;
        if (!CopyFromUser(&destination, destVal, sizeof(destination))) return -EFAULT;
    }
    if (lenVal > 65535) return -90; // EMSGSIZE
    auto* payload = static_cast<uint8_t*>(kmalloc(lenVal ? lenVal : 1));
    if (!payload) return -ENOMEM;
    if (lenVal && !CopyFromUser(payload, bufVal, lenVal)) { kfree(payload); return -EFAULT; }
    int ret = SockSendTo(sockIdx, payload, static_cast<uint32_t>(lenVal),
                          destVal ? &destination : nullptr);
    kfree(payload);
    if (ret < 0) return -EIO;
    return static_cast<int64_t>(lenVal);
}

int64_t sys_recvfrom(uint64_t fdVal, uint64_t bufVal, uint64_t lenVal,
                      uint64_t flagsVal, uint64_t srcVal, uint64_t addrLenVal)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (!fde || fde->type == FdType::Path) return -EBADF;
    if(fde->type==FdType::NetlinkSocket)return NetlinkReadUser(static_cast<NetlinkSocket*>(fde->handle),bufVal,lenVal,flagsVal,srcVal,addrLenVal);
    if (fde->type != FdType::UnixSocket && fde->type != FdType::Socket)
        return -ENOTSOCK;
    if (fde && fde->type == FdType::UnixSocket) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if(usd && usd->socketType==5) {
            if(flagsVal&~(0x40ULL|0x20ULL|0x4000ULL))return -95;
            if(lenVal && !UserBufferWritable(bufVal,lenVal))return -EFAULT;
            UnixFdSnap descriptors[UNIX_FD_QUEUE_CAP];int received=0;uint64_t size=0;
            auto result=UnixSocketReadSome(usd,(flagsVal&0x40)||(FdStatus(fde)&0x800),
                reinterpret_cast<char*>(bufVal),lenVal,descriptors,received,nullptr,&size);
            for(int i=0;i<received;++i)UnixFdSnapRelease(&descriptors[i]);
            // The peer is unnamed: report a zero-length address.
            const uint32_t unnamed=0;
            if(result>=0 && srcVal && addrLenVal && !CopyToUser(addrLenVal,&unnamed,sizeof(unnamed)))return -EFAULT;
            return result<0?result:(flagsVal&0x20)?static_cast<int64_t>(size):result;
        }
        // The nested read's lease ends before the address-length store below.
        UserAccessScope addressAccess(proc);
        if(srcVal && addrLenVal &&
           !UserBufferWritable(addrLenVal,sizeof(uint32_t)))return -EFAULT;
        bool oldNonblock = usd ? usd->nonblock : false;
        if (usd && (flagsVal & 0x40)) // MSG_DONTWAIT
            usd->nonblock = true;
        int64_t ret = sys_read(fdVal, bufVal, lenVal, 0, 0, 0);
        if (usd)
            usd->nonblock = oldNonblock;
        const uint32_t unnamed = 0;
        if (ret >= 0 && srcVal && addrLenVal && !CopyToUser(addrLenVal, &unnamed, sizeof(unnamed)))
            return -EFAULT;
        return ret;
    }

    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    // For TCP sockets, use SockRecv (stream receive)
    if (brook::SockIsStream(sockIdx))
    {
        const bool nonblock = (flagsVal & 0x40) || brook::SockIsNonblock(sockIdx);
        return ReceiveTcpToUser(sockIdx, bufVal, lenVal, nonblock);
    }

    // UDP path: payload and source address are staged in kernel memory.
    if (srcVal && !addrLenVal) return -EFAULT;
    SockAddrIn source {};
    const int64_t ret = ReceiveUdpToUser(sockIdx, bufVal, lenVal, &source);
    if (ret == -EFAULT || ret == -ENOMEM) return ret;
    if (ret < 0) return -EAGAIN;
    if (srcVal) {
        if (int64_t error = CopySockAddrToUser(srcVal, addrLenVal, &source, sizeof(source)))
            return error;
    }
    return ret;
}

int64_t sys_setsockopt(uint64_t fdVal, uint64_t levelVal, uint64_t optionVal,
                        uint64_t valueAddr, uint64_t length, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, static_cast<int>(fdVal));
    if (!descriptor) return -EBADF;
    if(descriptor->type==FdType::NetlinkSocket) {
        if(levelVal!=1 || optionVal!=8)return -92;
        int32_t value=0;if(length<4)return -EINVAL;if(!CopyFromUser(&value,valueAddr,4))return -EFAULT;
        if(value<0)return -EINVAL;
        uint32_t limit=static_cast<uint32_t>(value);if(limit<4096)limit=4096;if(limit>1048576)limit=1048576;
        auto* socket=static_cast<NetlinkSocket*>(descriptor->handle);
        SpinLockAcquire(&socket->lock);socket->receiveLimit=2*limit;SpinLockRelease(&socket->lock);return 0;
    }
    if (descriptor->type != FdType::UnixSocket && descriptor->type != FdType::Socket) return -88;
    if (descriptor->type == FdType::UnixSocket && levelVal == 1 && optionVal == 16) {
        if (length < sizeof(int32_t)) return -EINVAL;
        if (!UserBufferReadable(valueAddr, sizeof(int32_t))) return -EFAULT;
        auto* socket = static_cast<UnixSocketData*>(descriptor->handle);
        __atomic_store_n(&socket->passCredentials,
            *reinterpret_cast<const int32_t*>(valueAddr) != 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 0; // Existing unsupported socket options still use the legacy stub.
}

int64_t sys_getsockopt(uint64_t fdVal, uint64_t levelVal, uint64_t optnameVal,
                        uint64_t optvalAddr, uint64_t optlenAddr, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, static_cast<int>(fdVal));
    if (!descriptor) return -EBADF;
    if(descriptor->type==FdType::NetlinkSocket) {
        if(levelVal!=1)return -92;
        auto* socket=static_cast<NetlinkSocket*>(descriptor->handle);int32_t value=0;
        switch(optnameVal){case 3:value=socket->type;break;case 4:value=0;break;case 8:SpinLockAcquire(&socket->lock);value=socket->receiveLimit;SpinLockRelease(&socket->lock);break;case 38:value=0;break;case 39:value=16;break;default:return -92;}
        uint32_t length=0;if(!CopyFromUser(&length,optlenAddr,4))return -EFAULT;if(length>0x7fffffffu)return -EINVAL;
        uint32_t copied=length<4?length:4;
        if((copied && !CopyToUser(optvalAddr,&value,copied)) || !CopyToUser(optlenAddr,&copied,4))return -EFAULT;return 0;
    }
    FdEntry* entry = descriptor.get();
    if (entry->type != FdType::UnixSocket && entry->type != FdType::Socket)
        return -88; // ENOTSOCK
    if (!entry->handle) return -EBADF;
    int level = static_cast<int>(levelVal);
    int option = static_cast<int>(optnameVal);
    if (level == 6 && entry->type == FdType::Socket) { // IPPROTO_TCP
        TcpInfoSource source{};
        const int index = static_cast<int>(reinterpret_cast<uintptr_t>(entry->handle)) - 1;
        // Linux: a UDP socket passes SOL_TCP to ip_getsockopt (EOPNOTSUPP);
        // TCP knows only TCP_INFO here (other options ENOPROTOOPT).
        if (!brook::SockGetTcpInfo(index, &source)) return -95;
        if (option != 11) return -92;
        int32_t length = 0;
        if (!CopyFromUser(&length, optlenAddr, sizeof(length))) return -EFAULT;
        if (length < 0) return -EINVAL;
        const LinuxTcpInfo info = TcpInfoEncode(source);
        const uint32_t copied = static_cast<uint32_t>(length) < sizeof(info) ? static_cast<uint32_t>(length) : sizeof(info);
        if (!CopyToUser(optlenAddr, &copied, sizeof(copied))) return -EFAULT;
        if (copied && !CopyToUser(optvalAddr, &info, copied)) return -EFAULT;
        return 0;
    }
    if (level != 1) return entry->type == FdType::UnixSocket ? -95 : -92;
    // Linux socklen_t is 32-bit, and negative lengths are invalid.
    if (!UserBufferReadable(optlenAddr, sizeof(uint32_t)) ||
        !UserBufferWritable(optlenAddr, sizeof(uint32_t))) return -EFAULT;
    uint32_t requested = *reinterpret_cast<const uint32_t*>(optlenAddr);
    if (requested > 0x7fffffffu) return -EINVAL;
    uint32_t valueSize = entry->type == FdType::UnixSocket && option == 17
        ? sizeof(UnixCredentials) : sizeof(int32_t);
    uint32_t copied = requested < valueSize ? requested : valueSize;
    if (copied && !UserBufferWritable(optvalAddr, copied)) return -EFAULT;

    int32_t value = 0;
    UnixCredentials credentials{};
    if (entry->type == FdType::UnixSocket) {
        auto* socket = static_cast<UnixSocketData*>(entry->handle);
        switch (option) {
        case 16: // SO_PASSCRED
            value = __atomic_load_n(&socket->passCredentials, __ATOMIC_ACQUIRE) ? 1 : 0;
            break;
        case 17: // SO_PEERCRED (struct ucred), immutable connection identity.
            SpinLockAcquire(&g_unixRegistryLock);
            credentials = socket->peerCredentials;
            SpinLockRelease(&g_unixRegistryLock);
            break;
        case 3: // SO_TYPE: type selected at socket/socketpair creation.
            value = static_cast<int32_t>(socket->socketType);
            break;
        case 4: // SO_ERROR: Unix connect currently reports failures synchronously.
            value = 0;
            break;
        case 38: // SO_PROTOCOL
            value = 0;
            break;
        case 39: // SO_DOMAIN
            value = AF_UNIX;
            break;
        case 7: // SO_SNDBUF
        case 8: { // SO_RCVBUF
            PipeBuffer* pipe = option == 7 ? socket->txPipe : socket->rxPipe;
            if (!pipe || !pipe->capacity) return -ENOTCONN;
            // Ring buffers reserve one byte to distinguish full from empty.
            value = static_cast<int32_t>(pipe->capacity - 1);
            break;
        }
        default: return -92; // ENOPROTOOPT, never success with untouched output.
        }
    } else {
        int index = static_cast<int>(reinterpret_cast<uintptr_t>(entry->handle)) - 1;
        switch (option) {
        case 3: // SO_TYPE: required by runtimes before wrapping a TCP socket in TLS.
            value = brook::SockGetType(index);
            if (value < 0) return -EBADF;
            break;
        case 4: // SO_ERROR: retain the existing async-connect error semantics.
            value = brook::SockGetConnectError(index);
            break;
        case 38: // SO_PROTOCOL: report the resolved creation protocol.
            value = brook::SockGetProtocol(index);
            if (value < 0) return -EBADF;
            break;
        case 39: // SO_DOMAIN: Internet sockets currently support only AF_INET.
            if (brook::SockGetType(index) < 0) return -EBADF;
            value = AF_INET;
            break;
        default: return -92; // ENOPROTOOPT
        }
    }
    if(valueSize==sizeof(credentials)) {
        credentials.uid=ProcessReportId(proc,false,credentials.uid);credentials.gid=ProcessReportId(proc,true,credentials.gid);
    }
    if (copied) memcpy(reinterpret_cast<void*>(optvalAddr),
                       valueSize == sizeof(credentials) ? static_cast<const void*>(&credentials)
                                                        : static_cast<const void*>(&value), copied);
    *reinterpret_cast<uint32_t*>(optlenAddr) = copied;
    return 0;
}

static int64_t UnixSocketName(UnixSocketData* socket,bool peer,uint64_t addrVal,uint64_t addrLenVal)
{
    uint8_t address[sizeof(SockAddrUn)]={1,0};uint32_t actual=2;
    SpinLockAcquire(&g_unixRegistryLock);
    if(peer && socket->state!=UnixSocketData::State::Connected) {
        SpinLockRelease(&g_unixRegistryLock);return -ENOTCONN;
    }
    const uint32_t length=peer?socket->peerAddressLength:socket->localAddressLength;
    if(length) {
        actual=length;
        memcpy(address,peer?socket->peerAddress:socket->localAddress,length);
    }
    SpinLockRelease(&g_unixRegistryLock);
    return CopySockAddrToUser(addrVal,addrLenVal,address,actual);
}

int64_t sys_getsockname(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc,static_cast<int>(fdVal));
    if (!descriptor) return -EBADF;
    if(descriptor->type==FdType::NetlinkSocket)return NetlinkCopyName(__atomic_load_n(&static_cast<NetlinkSocket*>(descriptor->handle)->port,__ATOMIC_ACQUIRE),addrVal,addrLenVal);
    if (descriptor->type==FdType::UnixSocket && descriptor->handle) {
        return UnixSocketName(static_cast<UnixSocketData*>(descriptor->handle),false,addrVal,addrLenVal);
    }

    int fd = static_cast<int>(fdVal);
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    if (!addrVal || !addrLenVal) return -EFAULT;

    brook::SockAddrIn local{};
    local.sin_family = AF_INET;
    uint32_t tmpIp = 0;
    uint16_t tmpPort = 0;
    brook::SockGetLocal(sockIdx, &tmpIp, &tmpPort);
    local.sin_addr = tmpIp;
    local.sin_port = tmpPort;

    return CopySockAddrToUser(addrVal, addrLenVal, &local, sizeof(local));
}

int64_t sys_getpeername(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                         uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();

    if (!proc) return -ENOSYS;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc,static_cast<int>(fdVal));
    if(!descriptor)return -EBADF;
    if(descriptor->type==FdType::NetlinkSocket){if(!__atomic_load_n(&static_cast<NetlinkSocket*>(descriptor->handle)->connected,__ATOMIC_ACQUIRE))return -ENOTCONN;return NetlinkCopyName(0,addrVal,addrLenVal);}
    if(descriptor->type==FdType::UnixSocket && descriptor->handle)
        return UnixSocketName(static_cast<UnixSocketData*>(descriptor->handle),true,addrVal,addrLenVal);
    if(descriptor->type!=FdType::Socket)return -ENOTSOCK;

    // For inet DGRAM sockets that are "connected" via connect(), return the peer address
    int sockIdx=static_cast<int>(reinterpret_cast<uintptr_t>(descriptor->handle))-1;
    if (sockIdx >= 0) {
        uint32_t remIp = 0;
        uint16_t remPort = 0;
        brook::SockGetRemote(sockIdx, &remIp, &remPort);
        if (remIp != 0 || remPort != 0) {
            if (!addrVal || !addrLenVal) return -EFAULT;
            brook::SockAddrIn peer{};
            peer.sin_family = AF_INET;
            peer.sin_addr = remIp;
            peer.sin_port = remPort;
            return CopySockAddrToUser(addrVal, addrLenVal, &peer, sizeof(peer));
        }
    }

    return -ENOTCONN;
}

int64_t sys_shutdown(uint64_t fdVal, uint64_t howVal, uint64_t,
                      uint64_t, uint64_t, uint64_t)
{
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef fd(proc,static_cast<int>(fdVal));if(!fd)return -EBADF;
    if(fd->type!=FdType::UnixSocket && fd->type!=FdType::Socket && fd->type!=FdType::NetlinkSocket)return -88; // ENOTSOCK
    const int how=static_cast<int>(howVal);if(how<0 || how>2)return -EINVAL;
    // Internet socket shutdown needs a separate TCP/UDP state transition.
    if(fd->type!=FdType::UnixSocket)return -95; // EOPNOTSUPP
    auto* socket=static_cast<UnixSocketData*>(fd->handle);
    // Unbound/listening Unix shutdown also has Linux semantics, but is outside
    // this connected-stream implementation. Do not silently claim success.
    if(!socket || socket->state!=UnixSocketData::State::Connected || !socket->rxPipe || !socket->txPipe)
        return -95; // EOPNOTSUPP
    if(how==0 || how==2)UnixShutdownDirection(socket->rxPipe,false);
    if(how==1 || how==2)UnixShutdownDirection(socket->txPipe,true);
    return 0;
}

int64_t sys_listen(uint64_t fdVal, uint64_t backlog, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX: mark socket as listening and register in global table
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if(fde && fde->type==FdType::NetlinkSocket)return -95;
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        UnixNameGuard nameGuard;
        if (usd->state == UnixSocketData::State::Connected) return -EINVAL;
        if (!usd->localAddressLength) return -EINVAL;
        // A second socket bound to the same abstract name before either
        // listened is caught here.
        if (UnixAbstractNameInUse(usd->localAddress, usd->localAddressLength, usd)) return -98; // EADDRINUSE
        SpinLockAcquire(&g_unixRegistryLock);
        // Linux captures listener identity on the first listen, not at accept.
        if (usd->state != UnixSocketData::State::Listening) {
            usd->peerCredentials = UnixCurrentCredentials(proc);
            usd->peerIncarnation=UnixCurrentIncarnation(proc);usd->peerLaunchId=proc->launchId;
        }
        usd->state = UnixSocketData::State::Listening;
        SpinLockRelease(&g_unixRegistryLock);
        if (!UnixRegisterServer(usd)) return -ENOMEM;
        UNIX_LIFE(Listen, usd, 0, 0, fd);
        SerialPrintf("sys_listen: AF_UNIX fd=%d path='%s'\n", fd, usd->path);
        return 0;
    }

    (void)backlog;
    int sockIdx = GetSockIdx(proc, fd);
    if (sockIdx < 0) return -EBADF;

    int ret = brook::SockListen(sockIdx, static_cast<int>(backlog));
    return ret < 0 ? -EINVAL : 0;
}

static int64_t AcceptSocket(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                            uint32_t flags)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);

    // AF_UNIX accept: dequeue a pending connection
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if(fde && fde->type==FdType::NetlinkSocket)return -95;
    // The peer address is written after the descriptor is published, so
    // check the destination first; a raced unmap afterwards cannot undo the
    // accept and the new descriptor is still returned.
    uint32_t requestedAddrLen = 0;
    if (addrVal) {
        if (!CopyFromUser(&requestedAddrLen, addrLenVal, sizeof(requestedAddrLen))) return -EFAULT;
        if (requestedAddrLen > 0x7fffffffu) return -EINVAL;
        if (!UserBufferWritable(addrLenVal, sizeof(uint32_t))) return -EFAULT;
        const uint32_t needed = requestedAddrLen < sizeof(SockAddrIn) ? requestedAddrLen : sizeof(SockAddrIn);
        if (needed && !UserBufferWritable(addrVal, needed)) return -EFAULT;
    }
    if (fde && fde->type == FdType::UnixSocket && fde->handle) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);
        if (usd->state != UnixSocketData::State::Listening) return -EINVAL;

        auto* serverSock = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
        if (!serverSock) return -ENOMEM;
        UnixLife(UnixLifeEvent::Allocate, serverSock, 0, 1, -1,
                 reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
        memset(serverSock, 0, sizeof(UnixSocketData));
        serverSock->refCount = 1;
        struct AcceptedAllocation {
            UnixSocketData* socket;
            ~AcceptedAllocation() { if (socket) UnixSocketHandleUnref(socket); }
        } allocation{serverSock};

        for (;;) {
            if (__atomic_load_n(&fde->closing, __ATOMIC_ACQUIRE)) return -EBADF;
            SpinLockAcquire(&g_unixRegistryLock);
            int slot = -1;
            for (int i = 0; i < UNIX_ACCEPT_QUEUE; ++i)
                if (usd->pending[i].used) { slot = i; break; }
            if (slot >= 0) {
                const UnixPendingConn& pending = usd->pending[slot];
                serverSock->state = UnixSocketData::State::Connected;
                serverSock->socketType = usd->socketType;
                serverSock->peerCredentials = pending.clientCredentials;
                serverSock->peerIncarnation=pending.clientIncarnation;
                serverSock->peerLaunchId=pending.clientLaunchId;serverSock->acceptedEndpoint=true;
                serverSock->localAddressLength=usd->localAddressLength;
                memcpy(serverSock->localAddress,usd->localAddress,sizeof(serverSock->localAddress));
                serverSock->peerAddressLength=pending.clientAddressLength;
                memcpy(serverSock->peerAddress,pending.clientAddress,sizeof(serverSock->peerAddress));
                serverSock->nonblock = (flags & UNIX_SOCK_NONBLOCK) != 0;
                serverSock->rxPipe = pending.serverRx;
                serverSock->txPipe = pending.serverTx;
                serverSock->incomingFds = pending.serverRxFds;
                serverSock->peerIncomingFds = pending.serverTxFds;

                // Registry -> fd lock is safe: finalizers release fdLock before
                // taking the registry lock, and fd retain callbacks only use
                // atomic reference increments. Keep the pending owner intact
                // until publication succeeds so EMFILE remains retryable.
                int newFd = UnixInstallSocket(proc, serverSock, flags, "accept");
                if (newFd < 0) {
                    serverSock->rxPipe = serverSock->txPipe = nullptr;
                    serverSock->incomingFds = serverSock->peerIncomingFds = nullptr;
                    SpinLockRelease(&g_unixRegistryLock);
                    return newFd;
                }
                usd->pending[slot] = {};
                --usd->pendingCount;
                allocation.socket = nullptr; // pending ownership transferred to fd
                SpinLockRelease(&g_unixRegistryLock);
                // The client is reported as unnamed (zero-length address).
                const uint32_t unnamed = 0;
                if (addrVal) (void)CopyToUser(addrLenVal, &unnamed, sizeof(unnamed));
                SerialPrintf("sys_accept: AF_UNIX fd=%d -> newFd=%d\n", fd, newFd);
                return newFd;
            }
            if (usd->nonblock) {
                SpinLockRelease(&g_unixRegistryLock);
                return -EAGAIN;
            }
            usd->acceptWaiter = proc;
            SpinLockRelease(&g_unixRegistryLock);
            proc->wakeupTick = ApicTickCount() + 100;
            SchedulerBlock(proc);
            SpinLockAcquire(&g_unixRegistryLock);
            if (usd->acceptWaiter == proc) usd->acceptWaiter = nullptr;
            SpinLockRelease(&g_unixRegistryLock);
            if (HasPendingSignals()) return -EINTR;
        }
    }

    if (!fde || fde->type != FdType::Socket || !fde->handle) return -EBADF;
    // Keep the original pinned slot/socket across waits; a fresh lookup can
    // fail once a concurrent close marks this slot closing.
    const int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
    // Linux allocates the new sockfs inode before a potentially blocking accept.
    const auto acceptCredentials = ProcessFileActorAcquire(proc, nullptr);
    const uint32_t acceptUid = acceptCredentials.uid, acceptGid = acceptCredentials.gid;
    ProcessGroupsRelease(acceptCredentials.groups);
    brook::SockAddrIn peerAddr = {};
    int childIdx;
    for (;;) {
        if (__atomic_load_n(&fde->closing, __ATOMIC_ACQUIRE)) return -EBADF;
        childIdx = brook::SockAccept(sockIdx, &peerAddr);
        if (childIdx != -EAGAIN) break;
        // accept4 flags apply to the new connection, never the listener wait.
        if (brook::SockIsNonblock(sockIdx)) return -EAGAIN;
        if (HasPendingSignals()) return -EINTR;
        struct ExactSocketWait {
            int index; Process* task;
            ~ExactSocketWait() { brook::SockClearPollWaiter(index, task); }
        } registration{sockIdx, proc};
        pollfd wait{fd, POLLIN, 0};
        // Existing poll registration/recheck/signal handling, with a finite
        // close check. Its numeric-FD cleanup skips closing slots; the exact
        // original-socket guard above must therefore run on every exit.
        const int64_t result = PollWait(&wait, 1, 50);
        if (result < 0) return result;
    }
    if (childIdx < 0) return childIdx;
    brook::SockSetAcceptedOwner(childIdx, acceptUid, acceptGid);
    brook::SockSetNonblock(childIdx, (flags & UNIX_SOCK_NONBLOCK) != 0);

    // Allocate a new fd for the accepted connection
    FdEntry accepted{};
    accepted.type = FdType::Socket;
    accepted.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(childIdx + 1));
    accepted.fdFlags = (flags & UNIX_SOCK_CLOEXEC) ? 1 : 0;
    brook::SocketIdentity identity{};
    if (!brook::SockGetIdentity(childIdx, &identity) ||
        !brook::SocketIdentityLabel(identity.inode, accepted.dirPath, sizeof(accepted.dirPath))) {
        brook::SockClose(childIdx); return -ENOMEM;
    }
    int newFd = FdPublishNew(proc,accepted,flags & UNIX_SOCK_NONBLOCK);
    if (newFd < 0) {
        brook::SockClose(childIdx);
        return -EMFILE;
    }

    // Copy peer address to user if requested: min(*addrlen, actual) bytes.
    if (addrVal)
        (void)CopySockAddrToUser(addrVal, addrLenVal, &peerAddr, sizeof(peerAddr));

    SerialPrintf("sys_accept: fd=%d -> newFd=%d childIdx=%d\n", fd, newFd, childIdx);
    return newFd;
}

int64_t sys_accept(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                   uint64_t, uint64_t, uint64_t)
{
    return AcceptSocket(fdVal, addrVal, addrLenVal, 0);
}

int64_t sys_accept4(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                     uint64_t flags, uint64_t, uint64_t)
{
    uint32_t options = static_cast<uint32_t>(flags);
    if (options & ~(UNIX_SOCK_NONBLOCK | UNIX_SOCK_CLOEXEC)) return -EINVAL;
    return AcceptSocket(fdVal, addrVal, addrLenVal, options);
}

int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol,
                        uint64_t svAddr, uint64_t, uint64_t)
{
    SerialPrintf("sys_socketpair: domain=%lu type=0x%lx proto=%lu sv=0x%lx\n",
                 domain, type, protocol, svAddr);
    // We only support AF_UNIX (domain=1) socketpairs
    if (domain != 1) return -EAFNOSUPPORT;

    auto* sv = reinterpret_cast<int32_t*>(svAddr);
    if (!UserBufferWritable(svAddr, 2 * sizeof(int32_t))) return -EFAULT;

    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;
    KernelOperationGuard operation(proc);

    // Allocate two PipeBuffers — one for each direction
    auto* pipeAtoB = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
    if (!pipeAtoB) return -ENOMEM;
    auto* pipeBtoA = PipeBufferCreate(PIPE_BUF_UNIX_SIZE);
    if (!pipeBtoA) { PipeBufferDestroy(pipeAtoB); return -ENOMEM; }
    auto* fdqAtoB = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
    if (!fdqAtoB) { PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }
    auto* fdqBtoA = static_cast<UnixFdQueue*>(kmalloc(sizeof(UnixFdQueue)));
    if (!fdqBtoA) { kfree(fdqAtoB); PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }
    auto* endA = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
    if (!endA) { kfree(fdqBtoA); kfree(fdqAtoB); PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }
    UnixLife(UnixLifeEvent::Allocate, endA, 0, 1, -1,
             reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
    auto* endB = static_cast<UnixSocketData*>(kmalloc(sizeof(UnixSocketData)));
    if (!endB) { UNIX_LIFE(DirectFree, endA, 1, 0, -1); kfree(endA); kfree(fdqBtoA); kfree(fdqAtoB); PipeBufferDestroy(pipeAtoB); PipeBufferDestroy(pipeBtoA); return -ENOMEM; }

    // Zero-init backing objects
    memset(fdqAtoB, 0, sizeof(UnixFdQueue));
    memset(fdqBtoA, 0, sizeof(UnixFdQueue));
    memset(endA, 0, sizeof(UnixSocketData));
    UnixLife(UnixLifeEvent::Allocate, endB, 0, 1, -1,
             reinterpret_cast<uint64_t>(__builtin_return_address(0)), sizeof(UnixSocketData));
    memset(endB, 0, sizeof(UnixSocketData));

    pipeAtoB->readers = 1;
    pipeAtoB->writers = 1;
    pipeBtoA->readers = 1;
    pipeBtoA->writers = 1;
    fdqAtoB->refCount = 2;
    fdqBtoA->refCount = 2;

    // fd[0]: reads from pipeBtoA, writes to pipeAtoB
    // fd[1]: reads from pipeAtoB, writes to pipeBtoA
    bool nonblock = (type & UNIX_SOCK_NONBLOCK) != 0;
    endA->state = UnixSocketData::State::Connected;
    endA->nonblock = nonblock;
    endA->refCount = 1;
    endA->socketType = static_cast<uint32_t>(type) & 0xfu;
    endA->peerCredentials = UnixCurrentCredentials(proc);
    endA->peerIncarnation=UnixCurrentIncarnation(proc);endA->peerLaunchId=proc->launchId;
    endA->rxPipe = pipeBtoA;
    endA->txPipe = pipeAtoB;
    endA->incomingFds = fdqBtoA;
    endA->peerIncomingFds = fdqAtoB;

    endB->state = UnixSocketData::State::Connected;
    endB->nonblock = nonblock;
    endB->refCount = 1;
    endB->socketType = static_cast<uint32_t>(type) & 0xfu;
    endB->peerCredentials = endA->peerCredentials;
    endB->peerIncarnation=endA->peerIncarnation;endB->peerLaunchId=endA->peerLaunchId;
    endB->rxPipe = pipeAtoB;
    endB->txPipe = pipeBtoA;
    endB->incomingFds = fdqAtoB;
    endB->peerIncomingFds = fdqBtoA;

    int fdA = UnixInstallSocket(proc, endA, static_cast<uint32_t>(type), "socketpair-a");
    if (fdA < 0) {
        UnixSocketHandleUnref(endA);
        UnixSocketHandleUnref(endB);
        return fdA;
    }
    int fdB = UnixInstallSocket(proc, endB, static_cast<uint32_t>(type), "socketpair-b");
    if (fdB < 0) {
        CloseProcessFd(proc, fdA);
        UnixSocketHandleUnref(endB);
        return fdB;
    }

    sv[0] = fdA;
    sv[1] = fdB;

    SerialPrintf("sys_socketpair: fd[%d,%d] for pid %u\n", fdA, fdB, proc->pid);
    return 0;
}

// Linux x86-64 ABI constants (match <sys/socket.h>).
static constexpr int SOL_SOCKET  = 1;
static constexpr int SCM_RIGHTS  = 1;

struct CmsgHdr {
    uint64_t cmsg_len;   // socklen_t in struct, but aligned to 8 bytes
    int      cmsg_level;
    int      cmsg_type;
    // followed by data, padded to 8
};

// CMSG alignment rounds to sizeof(size_t) = 8 on x86-64.
static inline uint64_t CmsgAlign(uint64_t n) { return (n + 7) & ~uint64_t{7}; }

// Take an FdEntry snapshot suitable for passing over AF_UNIX. Like dup(), the
// snapshot names the SAME open file description (shared offset and status
// flags, H-20) and owns a handle reference retained by the common dup path,
// so every fd type dup() supports can be passed and the sender may close its
// fd before the receiver installs it (L-34).
static bool UnixFdSnapFrom(const FdEntry* src, UnixFdSnap* out)
{
    if (!src || src->type == FdType::None) return false;
    FdEntry copy = *src;
    if (!FdRetainCopy(&copy)) return false;
    out->type    = copy.type;
    out->flags   = copy.flags;
    out->fdFlags = copy.fdFlags;
    out->file    = copy.file;
    out->handle  = copy.handle;
    return true;
}

// Install a snapshot into the receiver's fd table. On success the snap's
// refcount becomes the new FdEntry's ref; on failure the caller must
// release it (drainSnap) so we don't leak.
static int UnixFdSnapInstall(Process* dst, const UnixFdSnap* snap)
{
    FdEntry owned{};
    owned.type = snap->type;
    owned.handle = snap->handle;
    owned.flags = snap->flags;
    owned.fdFlags = snap->fdFlags;
    owned.file = snap->file; // the receiver shares the sender's description
    if(owned.type==FdType::NetlinkSocket)SocketIdentityLabel(static_cast<NetlinkSocket*>(owned.handle)->identity.inode,owned.dirPath,sizeof(owned.dirPath));
    if (owned.type == FdType::Socket) {
        // The queued snapshot owns a socket reference. Reconstruct its cached
        // identity before publication, just as socket() and accept() do.
        brook::SocketIdentity identity{};
        int index = static_cast<int>(reinterpret_cast<uintptr_t>(owned.handle)) - 1;
        if (!brook::SockGetIdentity(index, &identity) ||
            !brook::SocketIdentityLabel(identity.inode, owned.dirPath, sizeof(owned.dirPath)))
            return -EBADF;
    }
    return FdTablePublishOwned(dst->fds, ProcessFdLock(dst), owned, 0, -1, nullptr, UnixLifePublished);
}

void UnixFdSnapRelease(const UnixFdSnap* snap)
{
    // Exactly the references UnixFdSnapFrom took: the handle as dup() retains
    // it and the open file description (L-34: one teardown path).
    if (!snap) return;
    FinalizeClosedFd({snap->type, snap->handle, snap->flags, snap->file});
}

struct LinuxMsgHdr {
    void*    msg_name;
    uint32_t msg_namelen;
    uint32_t _pad0;
    struct { void* iov_base; uint64_t iov_len; }* msg_iov;
    uint64_t msg_iovlen;
    void*    msg_control;
    uint64_t msg_controllen;
    int      msg_flags;
};

struct LinuxMMsgHdr {
    LinuxMsgHdr msg_hdr;
    uint32_t    msg_len;
    uint32_t    _pad0;
};

int64_t sys_sendmsg(uint64_t fdVal, uint64_t msgVal, uint64_t flagsVal,
                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    int fd = static_cast<int>(fdVal);
    if (msgVal < 0x1000) return -EFAULT;

    LinuxMsgHdr stagedHeader{};
    if(!CopyFromUser(&stagedHeader,msgVal,sizeof(stagedHeader)))return -EFAULT;
    auto* msg=&stagedHeader;
    if (msg->msg_iovlen>1024 || (msg->msg_iovlen && !msg->msg_iov)) return -EINVAL;
    if (msg->msg_iovlen && !UserBufferReadable(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen*sizeof(*msg->msg_iov))) return -EFAULT;

    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if (!fde) return -EBADF;
    if(fde && fde->type==FdType::NetlinkSocket) {
        if(msg->msg_controllen)return -95;
        NetlinkVector vector;if(int e=vector.init(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen,true))return e;
        return NetlinkSend(static_cast<NetlinkSocket*>(fde->handle),reinterpret_cast<uint64_t>(vector.bytes),vector.total,flagsVal,reinterpret_cast<uint64_t>(msg->msg_name),msg->msg_namelen,false);
    }
    if (fde->type == FdType::UnixSocket) {

    auto* usd = static_cast<UnixSocketData*>(fde->handle);
    if(!usd || usd->state!=UnixSocketData::State::Connected || !usd->txPipe)return -ENOTCONN;
    if(usd->socketType==5 && (flagsVal&~(0x40ULL|0x80ULL|0x4000ULL)))return -95;
    // Validation must not enqueue an empty sequenced record.
    SpinLockAcquire(&usd->txPipe->lock);
    const bool closed=!usd->txPipe->readers || usd->txPipe->unixReadShutdown || usd->txPipe->unixWriteShutdown;
    SpinLockRelease(&usd->txPipe->lock);
    if(closed) {if(!(flagsVal&0x4000))ProcessSendSignal(proc,13);return -EPIPE;}

    // Parse SCM_RIGHTS cmsg (if any) BEFORE writing payload. If cmsg parsing
    // fails (bad pointer, wrong type, fd full) we refuse the whole sendmsg
    // rather than send orphaned payload — matches Linux kernel behaviour.
    UnixFdSnap pendingSnaps[UNIX_FD_QUEUE_CAP];
    int pendingCount = 0;
    UnixCredentials explicitCredentials{};bool hasExplicitCredentials=false;
    auto failControl=[&](int error)->int64_t {
        for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
        return error;
    };

    if (msg->msg_control && msg->msg_controllen >= sizeof(CmsgHdr)) {
        uint64_t ctlAddr = reinterpret_cast<uint64_t>(msg->msg_control);
        if (!UserBufferReadable(ctlAddr,msg->msg_controllen)) return -EFAULT;

        uint64_t off = 0;
        while (off + sizeof(CmsgHdr) <= msg->msg_controllen) {
            CmsgHdr cm{};
            if(!CopyFromUser(&cm,ctlAddr+off,sizeof(cm)))return failControl(-EFAULT);
            if(cm.cmsg_len<sizeof(cm) || cm.cmsg_len>msg->msg_controllen-off)
                return failControl(-EINVAL);
            if(cm.cmsg_level==SOL_SOCKET && cm.cmsg_type==2) { // SCM_CREDENTIALS
                if(cm.cmsg_len!=sizeof(cm)+sizeof(UnixCredentials))return failControl(-EINVAL);
                UnixCredentials requested{};
                if(!CopyFromUser(&requested,ctlAddr+off+sizeof(cm),sizeof(requested)))return failControl(-EFAULT);
                requested.uid=ProcessResolveId(proc,false,requested.uid);requested.gid=ProcessResolveId(proc,true,requested.gid);
                const auto users=ProcessReadIds(proc,false),groups=ProcessReadIds(proc,true);
                const int32_t sender=proc->tgid?proc->tgid:proc->pid;
                if(int error=UnixCredentialsCheck(requested,sender,users,groups))return failControl(error);
                explicitCredentials=requested;hasExplicitCredentials=true;
            }
            if (cm.cmsg_level == SOL_SOCKET && cm.cmsg_type == SCM_RIGHTS) {
                uint64_t dataLen = cm.cmsg_len - sizeof(CmsgHdr);
                uint64_t nFds    = dataLen / sizeof(int);

                for (uint64_t i = 0; i < nFds; i++) {
                    if (pendingCount >= UNIX_FD_QUEUE_CAP-1) {
                        for (int j = 0; j < pendingCount; j++)
                            UnixFdSnapRelease(&pendingSnaps[j]);
                        return -90; // EMSGSIZE: this group can never fit.
                    }
                    int transferredFd=0;
                    if(!CopyFromUser(&transferredFd,ctlAddr+off+sizeof(cm)+i*sizeof(int),sizeof(int)))return failControl(-EFAULT);
                    FdRef sourceDescriptor(proc, transferredFd);
                    FdEntry* srcFde = sourceDescriptor.get();
                    if (!srcFde || !UnixFdSnapFrom(srcFde, &pendingSnaps[pendingCount])) {
                        for (int j = 0; j < pendingCount; j++)
                            UnixFdSnapRelease(&pendingSnaps[j]);
                        return -EBADF;
                    }
                    pendingCount++;
                }
            }

            uint64_t step = CmsgAlign(cm.cmsg_len);
            if (step == 0) break;
            off += step;
        }
    }

    // Commit the rights with the first actual payload byte. Failed writes own
    // no published snapshots, so there is no rollback racing a receiver.
    if(usd->socketType==5) {
        UnixRecordVector stage;
        if(int error=stage.Init(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen,true))return failControl(error);
        auto result=UnixSocketWriteSome(usd,(flagsVal&0x40)||(FdStatus(fde)&0x800),
            stage.bytes,stage.length,pendingSnaps,pendingCount,(flagsVal&0x4000)!=0,
            hasExplicitCredentials?&explicitCredentials:nullptr);
        if(result<0)return failControl(static_cast<int>(result));
        return result; // The record, including empty payload rights, owns the snapshots.
    }
    int64_t total=0;
    for(uint64_t i=0;i<msg->msg_iovlen;++i) {
        auto& iov=msg->msg_iov[i];if(!iov.iov_len)continue;
        if(!UserBufferReadable(reinterpret_cast<uint64_t>(iov.iov_base),iov.iov_len)) {
            for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
            return total?total:-EFAULT;
        }
        auto n=UnixSocketWriteSome(usd,(flagsVal&0x40)||(FdStatus(fde)&0x800),
            static_cast<const char*>(iov.iov_base),iov.iov_len,pendingSnaps,pendingCount,(flagsVal&0x4000)!=0,
            hasExplicitCredentials?&explicitCredentials:nullptr);
        if(n>0) {pendingCount=0;total+=n;}
        if(n<0 || static_cast<uint64_t>(n)<iov.iov_len) {
            for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
            return total?total:n;
        }
    }
    for(int j=0;j<pendingCount;++j)UnixFdSnapRelease(&pendingSnaps[j]);
    return total;
    }

    if (fde->type != FdType::Socket || !fde->handle) return -EBADF;
    int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
    struct SendIovStage {
        iovec* entries = nullptr;
        ~SendIovStage() { if (entries) kfree(entries); }
    } stage;
    if (msg->msg_iovlen) {
        const uint64_t bytes = msg->msg_iovlen * sizeof(iovec);
        stage.entries = static_cast<iovec*>(kmalloc(bytes));
        if (!stage.entries) return -ENOMEM;
        if (!CopyFromUser(stage.entries, reinterpret_cast<uint64_t>(msg->msg_iov), bytes)) return -EFAULT;
    }

    if (msg->msg_iovlen == 1) {
        uint64_t buf = stage.entries[0].iov_base;
        uint32_t len = static_cast<uint32_t>(stage.entries[0].iov_len > 0x7ffff000ULL ? 0x7ffff000ULL : stage.entries[0].iov_len);
        if (!buf || len == 0) return -EINVAL;
        if (brook::SockIsStream(sockIdx)) return brook::SockSendUser(sockIdx, buf, len);
        return sys_sendto(fdVal, buf, len, flagsVal,
                          reinterpret_cast<uint64_t>(msg->msg_name), msg->msg_namelen);
    }

    uint64_t totalLen = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
        if (__builtin_add_overflow(totalLen, stage.entries[i].iov_len, &totalLen))
            return -EINVAL;
    }
    if (totalLen == 0 || totalLen > 65507) return -EINVAL;

    auto* tmp = static_cast<uint8_t*>(kmalloc(totalLen));
    if (!tmp) return -ENOMEM;
    uint64_t off = 0;
    for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
        uint64_t base = stage.entries[i].iov_base;
        uint64_t len = stage.entries[i].iov_len;
        if (!base && len != 0) {
            kfree(tmp);
            return -EFAULT;
        }
        if (len && !CopyFromUser(tmp + off, base, len)) {
            kfree(tmp);
            return -EFAULT;
        }
        off += len;
    }

    int64_t ret = brook::SockIsStream(sockIdx)
        ? brook::SockSend(sockIdx, tmp, static_cast<uint32_t>(totalLen))
        : sys_sendto(fdVal, reinterpret_cast<uint64_t>(tmp), totalLen, flagsVal,
                     reinterpret_cast<uint64_t>(msg->msg_name), msg->msg_namelen);
    kfree(tmp);
    return ret;
}

int64_t sys_recvmsg(uint64_t fdVal, uint64_t msgVal, uint64_t flagsVal,
                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ENOSYS;

    constexpr uint64_t MSG_DONTWAIT = 0x40;
    bool dontwait = (flagsVal & MSG_DONTWAIT) != 0;

    int fd = static_cast<int>(fdVal);
    if (msgVal < 0x1000) return -EFAULT;

    if (!UserBufferWritable(msgVal,sizeof(LinuxMsgHdr))) return -EFAULT;
    LinuxMsgHdr stagedHeader{};
    if(!CopyFromUser(&stagedHeader,msgVal,sizeof(stagedHeader)))return -EFAULT;
    auto* msg=&stagedHeader;
    if (msg->msg_iovlen>1024 || (msg->msg_iovlen && !msg->msg_iov)) return -EINVAL;
    if (msg->msg_iovlen && !UserBufferReadable(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen*sizeof(*msg->msg_iov))) return -EFAULT;

    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, fd);
    FdEntry* fde = descriptor.get();
    if(fde && fde->type==FdType::NetlinkSocket) {
        if(msg->msg_namelen>0x7fffffffu)return -EINVAL;
        if(msg->msg_name && !UserBufferWritable(reinterpret_cast<uint64_t>(msg->msg_name),msg->msg_namelen<12?msg->msg_namelen:12))return -EFAULT;
        NetlinkVector vector;if(int e=vector.init(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen,false))return e;
        uint32_t fullSize=0;auto n=NetlinkReceive(static_cast<NetlinkSocket*>(fde->handle),vector.bytes,vector.capacity,flagsVal,fullSize);
        if(n<0)return n;
        if(int e=vector.scatter(n))return e;
        RouteAddress source{16,0,0,0};
        if(msg->msg_name && msg->msg_namelen && !CopyToUser(reinterpret_cast<uint64_t>(msg->msg_name),&source,msg->msg_namelen<12?msg->msg_namelen:12))return -EFAULT;
        msg->msg_namelen=msg->msg_name?12:0;msg->msg_controllen=0;msg->msg_flags=fullSize>static_cast<uint64_t>(n)?0x20:0;
        if(!CopyToUser(msgVal,msg,sizeof(*msg)))return -EFAULT;
        return (flagsVal&0x20)?fullSize:n;
    }
    if (fde && fde->type == FdType::UnixSocket) {
        auto* usd = static_cast<UnixSocketData*>(fde->handle);

        // MSG_NOSIGNAL is also accepted on receive by Linux. Receives never
        // generate SIGPIPE; audio IPC uses it with MSG_CMSG_CLOEXEC.
        const bool record=usd->socketType==5;
        if(flagsVal&~(0x40ULL|0x4000ULL|0x40000000ULL|(record?0x20ULL:0)))return -95;
        if(msg->msg_controllen && !UserBufferWritable(reinterpret_cast<uint64_t>(msg->msg_control),msg->msg_controllen))return -EFAULT;
        const uint64_t capacity=msg->msg_controllen;
        UnixFdSnap descriptors[UNIX_FD_QUEUE_CAP];int received=0;
        int64_t total=0;
        uint64_t messageLength=0;bool consumed=false;
        const bool passCredentials = __atomic_load_n(&usd->passCredentials, __ATOMIC_ACQUIRE);
        UnixCredentials sender{};
        if(record) {
            UnixRecordVector stage;
            if(int error=stage.Init(reinterpret_cast<uint64_t>(msg->msg_iov),msg->msg_iovlen,false))return error;
            total=UnixSocketReadSome(usd,dontwait || (FdStatus(fde)&0x800),stage.bytes,stage.capacity,
                descriptors,received,passCredentials?&sender:nullptr,&messageLength,&consumed);
            if(total<0)return total;
            if(int error=stage.Scatter(total)) {
                for(int j=0;j<received;++j)UnixFdSnapRelease(&descriptors[j]);
                return error;
            }
        } else for(uint64_t i=0;i<msg->msg_iovlen;++i) {
            auto& iov=msg->msg_iov[i];if(!iov.iov_len)continue;
            if(!UserBufferWritable(reinterpret_cast<uint64_t>(iov.iov_base),iov.iov_len)) {
                for(int j=0;j<received;++j)UnixFdSnapRelease(&descriptors[j]);
                return total?total:-EFAULT;
            }
            int count=0;
            auto n=UnixSocketReadSome(usd,dontwait || total>0 || (FdStatus(fde)&0x800),
                static_cast<char*>(iov.iov_base),iov.iov_len,descriptors,count,passCredentials ? &sender : nullptr);
            received=count;
            if(n<0 && !total)return n;
            if(n>0)total+=n;
            if(n<=0 || static_cast<uint64_t>(n)<iov.iov_len || received || passCredentials)break;
        }
        msg->msg_flags=record && messageLength>static_cast<uint64_t>(total)?0x20:0;
        msg->msg_controllen=0;msg->msg_namelen=0;
        uint64_t controlUsed = 0;
        if (passCredentials && (total > 0 || consumed)) {
            sender.uid=ProcessReportId(proc,false,sender.uid);sender.gid=ProcessReportId(proc,true,sender.gid);
            const uint64_t required = sizeof(CmsgHdr) + sizeof(sender);
            if (capacity >= sizeof(CmsgHdr)) {
                auto* header = static_cast<CmsgHdr*>(msg->msg_control);
                uint64_t copied = capacity < required ? capacity : required;
                header->cmsg_len = copied;
                header->cmsg_level = SOL_SOCKET; header->cmsg_type = 2; // SCM_CREDENTIALS
                if (copied > sizeof(CmsgHdr)) memcpy(header + 1, &sender, copied - sizeof(CmsgHdr));
                controlUsed = CmsgAlign(copied); if (controlUsed > capacity) controlUsed = capacity;
                msg->msg_controllen = controlUsed;
            }
            if (capacity < required) msg->msg_flags |= 0x8;
        }
        const uint64_t remaining = capacity - controlUsed;
        uint64_t maxFds=remaining>=sizeof(CmsgHdr)?(remaining-sizeof(CmsgHdr))/sizeof(int):0;
        int installed=0;
        for(int i=0;i<received;++i) {
            if(static_cast<uint64_t>(installed)<maxFds) {
                descriptors[i].fdFlags=(flagsVal&0x40000000ULL)?1:0;
                int installedFd=UnixFdSnapInstall(proc,&descriptors[i]);
                if(installedFd>=0) {
                    auto* slots=reinterpret_cast<int*>(reinterpret_cast<uint64_t>(msg->msg_control)+controlUsed+sizeof(CmsgHdr));
                    slots[installed++]=installedFd;continue;
                }
            }
            msg->msg_flags|=0x8;UnixFdSnapRelease(&descriptors[i]);
        }
        if(installed) {
            auto* header=reinterpret_cast<CmsgHdr*>(reinterpret_cast<uint64_t>(msg->msg_control)+controlUsed);
            header->cmsg_len=sizeof(CmsgHdr)+installed*sizeof(int);
            header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
            uint64_t used=CmsgAlign(header->cmsg_len);
            msg->msg_controllen=controlUsed+(used<remaining?used:remaining);
        }
        if(!CopyToUser(msgVal,msg,sizeof(*msg)))return -EFAULT;
        return record && (flagsVal&0x20)?static_cast<int64_t>(messageLength):total;
    }

    if(!fde)return -EBADF;
    if(fde->type!=FdType::Socket)return -ENOTSOCK;
    int sockIdx=static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle))-1;
    if (sockIdx < 0) return -EBADF;

    if(brook::SockIsStream(sockIdx)) {
        // TCP has no datagram header. Stage one stream read, then scatter it
        // outside the socket lock; user faults must never hold that lock.
        if(flagsVal&~(0x40ULL|0x4000ULL|0x40000000ULL))return -95;
        struct Allocations {
            void* vectors=nullptr;
            void* bytes=nullptr;
            ~Allocations(){kfree(bytes);kfree(vectors);}
        } allocated;
        const uint64_t vectorBytes=msg->msg_iovlen*sizeof(*msg->msg_iov);
        if(vectorBytes) {
            allocated.vectors=kmalloc(static_cast<uint32_t>(vectorBytes));
            if(!allocated.vectors)return -ENOMEM;
            if(!CopyFromUser(allocated.vectors,reinterpret_cast<uint64_t>(msg->msg_iov),vectorBytes))return -EFAULT;
        }
        auto vectors=static_cast<decltype(msg->msg_iov)>(allocated.vectors);
        uint64_t capacity=0;
        for(uint64_t i=0;i<msg->msg_iovlen;++i)
            if(__builtin_add_overflow(capacity,vectors[i].iov_len,&capacity) || capacity>0x7fffffffffffffffULL)return -EINVAL;
        const uint32_t count=static_cast<uint32_t>(capacity<65536?capacity:65536);
        uint32_t remaining=count;
        for(uint64_t i=0;i<msg->msg_iovlen && remaining;++i) {
            const uint32_t bytes=static_cast<uint32_t>(vectors[i].iov_len<remaining?vectors[i].iov_len:remaining);
            if(bytes && !UserBufferWritable(reinterpret_cast<uint64_t>(vectors[i].iov_base),bytes))return -EFAULT;
            remaining-=bytes;
        }
        int result=0;
        if(count) {
            allocated.bytes=kmalloc(count);if(!allocated.bytes)return -ENOMEM;
            const bool nonblock=dontwait || brook::SockIsNonblock(sockIdx);
            result=brook::SockRecv(sockIdx,allocated.bytes,count,nonblock);
            if(result<0)return result;
            uint32_t copied=0;
            for(uint64_t i=0;i<msg->msg_iovlen && copied<static_cast<uint32_t>(result);++i) {
                const uint32_t left=static_cast<uint32_t>(result)-copied;
                const uint32_t bytes=static_cast<uint32_t>(vectors[i].iov_len<left?vectors[i].iov_len:left);
                if(bytes && !CopyToUser(reinterpret_cast<uint64_t>(vectors[i].iov_base),static_cast<uint8_t*>(allocated.bytes)+copied,bytes))return -EFAULT;
                copied+=bytes;
            }
        }
        msg->msg_flags=0;msg->msg_controllen=0;msg->msg_namelen=0;
        if(!CopyToUser(msgVal,msg,sizeof(*msg)))return -EFAULT;
        return result;
    }

    if(!msg->msg_iovlen)return -EINVAL;
    iovec first{};
    if(!CopyFromUser(&first,reinterpret_cast<uint64_t>(msg->msg_iov),sizeof(first)))return -EFAULT;
    if (!first.iov_base || first.iov_len == 0) return -EINVAL;
    const uint32_t len = static_cast<uint32_t>(first.iov_len < 65536 ? first.iov_len : 65536);

    brook::SockAddrIn srcAddr {};
    const int64_t ret = ReceiveUdpToUser(sockIdx, first.iov_base, len, &srcAddr);
    if (ret == -EFAULT || ret == -ENOMEM) return ret;
    if (ret < 0) return -EAGAIN;

    if (msg->msg_name && msg->msg_namelen >= sizeof(brook::SockAddrIn)) {
        if(!CopyToUser(reinterpret_cast<uint64_t>(msg->msg_name),&srcAddr,sizeof(srcAddr)))return -EFAULT;
        msg->msg_namelen = sizeof(brook::SockAddrIn);
    }

    msg->msg_flags = 0;
    msg->msg_controllen = 0;
    if(!CopyToUser(msgVal,msg,sizeof(*msg)))return -EFAULT;
    return ret;
}

int64_t sys_sendmmsg(uint64_t fdVal, uint64_t msgvecVal, uint64_t vlenVal,
                      uint64_t flagsVal, uint64_t, uint64_t)
{
    if (vlenVal == 0) return 0;
    if (vlenVal > 1024) vlenVal = 1024;
    if (!UserBufferWritable(msgvecVal, vlenVal * sizeof(LinuxMMsgHdr)))
        return -EFAULT;

    auto* vec = reinterpret_cast<LinuxMMsgHdr*>(msgvecVal);
    uint64_t sent = 0;
    for (uint64_t i = 0; i < vlenVal; i++) {
        int64_t ret = sys_sendmsg(fdVal, reinterpret_cast<uint64_t>(&vec[i].msg_hdr),
                                  flagsVal, 0, 0, 0);
        if (ret < 0)
            return sent ? static_cast<int64_t>(sent) : ret;
        vec[i].msg_len = static_cast<uint32_t>(ret);
        sent++;
    }
    return static_cast<int64_t>(sent);
}

// recvmmsg(2) — receive multiple messages in one syscall.
// Signature: int recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
//                         int flags, struct timespec *timeout);
// We ignore the timeout parameter for now (treat as non-blocking batch).
int64_t sys_recvmmsg(uint64_t fdVal, uint64_t msgvecVal, uint64_t vlenVal,
                      uint64_t flagsVal, uint64_t timeoutVal, uint64_t)
{
    (void)timeoutVal; // timeout not implemented
    if (vlenVal == 0) return 0;
    if (vlenVal > 1024) vlenVal = 1024;
    if (!UserBufferWritable(msgvecVal, vlenVal * sizeof(LinuxMMsgHdr)))
        return -EFAULT;

    auto* vec = reinterpret_cast<LinuxMMsgHdr*>(msgvecVal);
    uint64_t received = 0;
    for (uint64_t i = 0; i < vlenVal; i++) {
        // After the first message, use MSG_DONTWAIT to avoid blocking
        uint64_t flags = flagsVal;
        if (i > 0) flags |= 0x40; // MSG_DONTWAIT
        int64_t ret = sys_recvmsg(fdVal, reinterpret_cast<uint64_t>(&vec[i].msg_hdr),
                                  flags, 0, 0, 0);
        if (ret < 0)
            return received ? static_cast<int64_t>(received) : ret;
        vec[i].msg_len = static_cast<uint32_t>(ret);
        received++;
    }
    return static_cast<int64_t>(received);
}

} // namespace brook
