#pragma once

// syscall_unix_socket.h -- internal declarations shared from syscall_unix_socket.cpp.
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include "unix_lifetime_trace.h"

#include "unix_credentials.h"
#include "syscall_internal.h"

namespace brook {

static constexpr int AF_UNIX           = 1;
static constexpr int UNIX_SOCK_NONBLOCK = 0x800;
static constexpr int UNIX_SOCK_CLOEXEC  = 0x80000;
static constexpr int UNIX_ACCEPT_QUEUE  = 8;
static constexpr int UNIX_FD_QUEUE_CAP  = 64;

// Snapshot of an FdEntry passed across an AF_UNIX connection via SCM_RIGHTS.
// The handle and open file description references are taken on send (as by
// dup), transferred to the receiver on recv (no net refcount change), or
// dropped on socket close. The receiver shares the sender's description.
struct UnixFdSnap {
    FdType    type;
    uint8_t   flags;
    uint8_t   fdFlags;
    OpenFile* file;
    void*     handle;
};

UnixCredentials UnixCurrentCredentials(Process* process);
uint64_t UnixCurrentIncarnation(Process* process);

// FIFO of in-flight fds for one direction of a connected AF_UNIX pair.
// Shared between sender (enqueue) and receiver (dequeue).
struct UnixFdQueue {
    UnixFdSnap   msgs[UNIX_FD_QUEUE_CAP];
    uint64_t offsets[UNIX_FD_QUEUE_CAP];
    uint32_t records[UNIX_FD_QUEUE_CAP]; // credential-span slot for sequenced records
    // Sender identity belongs to bytes, not the socket's connection-time peer.
    // Forked children can send through an inherited endpoint.
    struct CredentialSpan { uint64_t end; UnixCredentials sender; } credentials[UNIX_FD_QUEUE_CAP];
    uint32_t credentialHead, credentialTail;
    uint64_t written, read;
    volatile int head;
    volatile int tail;
    volatile uint32_t refCount; // one ref per UnixSocketData pointing at us
};

struct UnixPendingConn {
    UnixCredentials clientCredentials;
    uint64_t clientIncarnation,clientLaunchId;
    uint8_t clientAddress[110];
    uint32_t clientAddressLength;
    PipeBuffer* serverRx;     // server reads this (client→server data)
    PipeBuffer* serverTx;     // server writes this (server→client data)
    UnixFdQueue* serverRxFds; // server reads these fds (client→server)
    UnixFdQueue* serverTxFds; // server writes these fds (server→client)
    // Owns one server endpoint and one reference to each ancillary queue until
    // accept transfers them, or listener teardown releases them. Registry lock
    // serializes publication, dequeue and teardown of these slots.
    bool        used;
};

struct UnixSocketData {
    enum class State : uint8_t { Unbound, Listening, Connected };
    State   state;
    bool    nonblock;
    bool    passCredentials; // SO_PASSCRED; accessed atomically through shared fds
    volatile uint32_t refCount; // +1 per fd pointing at this struct
    uint32_t socketType;       // creation type (without NONBLOCK/CLOEXEC flags)
    UnixCredentials peerCredentials; // connection/listen snapshot; registry lock protects copies
    uint64_t peerIncarnation,peerLaunchId;
    bool acceptedEndpoint; // Only accept(), never connect() or socketpair().
    bool gpuPeerAuthorized; // exact connected renderer was checked while attached
    char    path[108];
    Vnode* boundNode; // Owns an inode pin even after unlink; close never unlinks.
    uint64_t boundDev, boundIno;
    // Routing path may become the server's name during connect. Preserve the
    // locally bound address separately for getsockname and accepted endpoints.
    uint8_t localAddress[110];
    uint32_t localAddressLength;
    uint8_t peerAddress[110];
    uint32_t peerAddressLength;
    UnixPendingConn pending[UNIX_ACCEPT_QUEUE];
    int             pendingCount;
    Process*        acceptWaiter;
    // Process inside epoll_wait watching this listening socket for readable
    // (= new pending connection). Cleared + SchedulerUnblock'd on connect.
    Process* volatile epollWaiter;
    PipeBuffer* rxPipe;
    PipeBuffer* txPipe;
    UnixFdQueue* incomingFds; // fds arriving on rxPipe
    UnixFdQueue* peerIncomingFds; // fds we post to, drained by peer's recvmsg
};

struct SockAddrUn {
    uint16_t sun_family; // AF_UNIX = 1
    char     sun_path[108];
};

extern SpinLock g_unixRegistryLock;

void UnixLife(UnixLifeEvent event, void* handle, uint32_t previous,
              uint32_t next, int fd, uint64_t caller, uint64_t size = 0);

#define UNIX_LIFE(event, object, before, after, fd) \
    UnixLife(UnixLifeEvent::event, object, before, after, fd, \
             reinterpret_cast<uint64_t>(__builtin_return_address(0)))

extern KMutex g_unixNameLock;

struct UnixNameGuard {
    UnixNameGuard() { KMutexLock(&g_unixNameLock); }
    ~UnixNameGuard() { KMutexUnlock(&g_unixNameLock); }
};

// Abstract names (sun_path[0] == 0) are the whole address, including any
// NUL bytes, compared with its length; they never match a filesystem name.
inline bool UnixAddressIsAbstract(const uint8_t* address, uint32_t length)
{
    return length > 2 && address[2] == 0;
}
// A filesystem socket is matched by inode; an abstract one by its full
// address bytes. Pins and returns the listener, or nullptr.
UnixSocketData* UnixFindServer(const char* path, const VnodeStat* node = nullptr,
                               const uint8_t* abstractAddress = nullptr,
                               uint32_t abstractLength = 0);
// Callers hold g_unixNameLock, so a check followed by bind or listen is
// atomic with respect to other name operations.
bool UnixAbstractNameInUse(const uint8_t* address, uint32_t length, const UnixSocketData* except);
bool UnixRegisterServer(UnixSocketData* usd);
void UnixSocketHandleUnref(void* handle);

// Both endpoints of a socketpair, published together or not at all.
int UnixInstallSocketPair(Process* process, UnixSocketData* first, UnixSocketData* second,
                          uint32_t flags, int out[2]);
// accept(): publish with a description created before the registry lock.
int UnixInstallSocketPrepared(Process* process, UnixSocketData* socket, uint32_t flags, OpenFile* file);
int UnixInstallSocket(Process* process, UnixSocketData* socket, uint32_t flags,
                      const char* site);

void UnixShutdownDirection(PipeBuffer* pipe, bool writer);
uint32_t UnixSocketReady(UnixSocketData* socket,uint32_t events);

// Same for a listening AF_UNIX socket: wake epoll waiter when a new
// connection is posted to the pending queue.
static inline void UnixListenWakeEpoll(UnixSocketData* usd)
{
    if (!usd) return;
    Process* ew = usd->epollWaiter;
    if (ew) {
        usd->epollWaiter = nullptr;
        WakeProcess(ew);
    }
}

int64_t UnixSocketWriteSome(UnixSocketData* socket,bool nonblock,
                            const char* bytes,uint64_t length,
                            const UnixFdSnap* descriptors=nullptr,int count=0,bool noSignal=false,
                            const UnixCredentials* explicitCredentials=nullptr);

int64_t UnixSocketReadSome(UnixSocketData* socket,bool nonblock,
                           char* bytes,uint64_t length,UnixFdSnap* descriptors,int& count,
                           UnixCredentials* sender=nullptr,uint64_t* recordLength=nullptr,bool* consumed=nullptr);

} // namespace brook
