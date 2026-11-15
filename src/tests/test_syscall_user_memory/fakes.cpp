// Minimal stand-ins for subsystems underneath the handlers under test. Most
// fail or report "absent"; the fake network stack is observable via g_net.
#include "harness.h"
#include "syscall_internal.h"
#include "syscall_drm.h"
#include "syscall_memfd.h"
#include "syscall_pipe.h"
#include "syscall_poll.h"
#include "syscall_unix_socket.h"
#include "syscall_netlink.h"
#include "syscall_timerfd.h"
#include "signal_fd.h"
#include "process_fs.h"
#include "fs_context.h"
#include "vfs_path.h"
#include "gpu_app.h"
#include "audio.h"
#include "input.h"
#include "window.h"
#include "tty.h"
#include "debug_overlay.h"
#include "guard_checker_session.h"
#include "intel_bdw_file.h"
#include "intel_bdw_buffer.h"
#include "compositor_scene.h"
#include "memory/mapping_ranges.h"
#include "memory/physical_memory.h"
#include "process_user_namespace.h"
#include "unix_lifetime_trace.h"

namespace harness {
FakeNet g_net;
unsigned g_snapshots;
}
using harness::g_net;

namespace brook {
// Filesystem: every lookup fails, so open stops at the first one.
int ProcessFsSnapshot(Process*,FsSnapshot*) { ++harness::g_snapshots;return -ENOENT; }
void FsSnapshotRelease(FsSnapshot*) {}
void VfsPathRetain(const VfsPath&) {}
void VfsPathRelease(VfsPath) {}
bool VfsPathSame(const VfsPath&,const VfsPath&) { return false; }
int VfsPathBrokerAllowed(const VfsPath&,const char*,bool*) { return -ENOENT; }
int VfsCanonicalDeviceAllowed(const VfsPath&,const VfsPath&,const char*,bool*,bool,const char*) { return -ENOENT; }
int VfsCanonicalDrmBrokerAllowed(const VfsPath&,bool,bool*,bool) { return -ENOENT; }
int VfsPathDeviceActivation(const VfsPath&,const char*) { return -ENOENT; }
int VfsPathFromVnode(Vnode*,VfsPath*) { return -ENOENT; }
int VfsPathResolve(const VfsPath&,const VfsPath&,const char*,VfsPath*,bool,bool) { return -ENOENT; }
int VfsPathDirectoryPath(const VfsPath&,char*,uint32_t) { return -ENOENT; }
Vnode* VfsOpenAt(const VfsPath&,const VfsPath&,const char*,int,uint16_t,uint16_t,int* error,bool) { if(error)*error=-ENOENT;return nullptr; }
Vnode* VfsCreateSocketAt(const VfsPath&,const VfsPath&,const char*,uint16_t,uint16_t,int* error) { if(error)*error=-ENOENT;return nullptr; }
void VfsRelease(Vnode*) {}
int VfsStat(Vnode*,VnodeStat*) { return -ENOENT; }
Vnode* GuardCheckerOpenLinux(uint64_t,int* error) { *error=-13;return nullptr; }
int NamedFifoAttach(Vnode*,uint32_t) { return -1; }
NamedFifoOpen* FdNamedFifo(const FdEntry*) { return nullptr; }
PipeBuffer* FdPipe(const FdEntry*) { return nullptr; }
bool FdPipeRead(const FdEntry*) { return false; }
void PipePollNotify(PipeBuffer*,uint32_t) {}
void PipeReleaseEnd(PipeBuffer*,bool) {}

// Devices and GPU: absent.
const char* DrmSysAttrContent(const char*) { return nullptr; }
bool DrmSysIsDir(const char*) { return false; }
const char* IntelDrmSysAttrContent(const char*) { return nullptr; }
bool IntelDrmSysIsDir(const char*) { return false; }
void DrmOwnerUnref(DrmGpuOwner*) {}
const char g_drmDriDirTag[]="dri";
const GpuAppOps* GpuAppGet() { return nullptr; }
bool BdwIntelUserNodeEnabled() { return false; }
BdwGpuFile* BdwGpuFileCreate() { return nullptr; }
void BdwGpuFileUnref(BdwGpuFile*) {}
void BdwGpuBufferUnref(BdwGpuBuffer*) {}
void BdwGpuSyncUnref(BdwGpuSync*) {}
void SyncFileUnref(SyncFileData*) {}
void SceneHandleUnref(SceneHandle*) {}
void SignalFdUnref(SignalFd*) {}
void MappingBackingUnref(MappingBacking*) {}
void PmmFreePage(PhysicalAddress) {}
void FinalizeClosedFd(const FdClaimResult&) {}
int FdAlloc(Process*,FdType,void*) { return -1; }
int MemFdInstall(Process*,MemFdData*,uint32_t) { return -1; }
int64_t MemFdReadAt(MemFdData*,void*,uint64_t,uint64_t*) { return -EBADF; }
int NamedShmOpen(Process*,const char*,uint32_t) { return -1; }
AudioStreamId AudioMixerOpen() { return 0; }
bool AudioMixerClose(AudioStreamId) { return true; }
bool AudioMixerSetCapacity(AudioStreamId,uint32_t,uint32_t) { return false; }
bool AudioAvailable() { return false; }
uint64_t ClockNowNs(uint64_t) { return 0; }
uint32_t DebugOverlayReadFrom(uint32_t*,char*,uint32_t,uint32_t) { return 0; }
void InputAddWaiter(Process*) {}
void InputRemoveWaiter(Process*) {}
void TtyPutChar(char) {}
bool WmIsActive() { return true; }
int64_t SignalFdRead(Process*,SignalFd*,uint64_t,uint64_t,bool) { return -EBADF; }
uint64_t TimerFdDeadline(TimerFdData*,uint64_t*) { return 0; }
uint64_t WaitWakeupTick(uint64_t,uint64_t) { return 0; }
int64_t PollWait(pollfd*,uint64_t,uint64_t,bool) { return -EINTR; }
uint32_t NetInterfaceViews(InterfaceView*,uint32_t) { return 0; }
FileActorSnapshot ProcessFileActorAcquire(Process*,UserNamespace*,bool) { return {}; }
void ProcessGroupsRelease(SupplementaryGroups*) {}
CredentialIds ProcessReadIds(Process*,bool) { return {}; }
int ProcessSendSignal(Process*,int,const SignalInfo*) { return 0; }
int VfsAccess(Vnode*,uint8_t,bool) { return -13; }
uint32_t ProcessReportId(Process*,bool,uint32_t canonical) { return canonical; }

// Inet sockets: one fake UDP/TCP socket, observable through g_net. The
// stack only ever receives kernel buffers.
bool SockIsNonblock(int) { return true; }
bool SockIsStream(int) { return false; }
int SockBind(int,const SockAddrIn* address) { ++g_net.binds;g_net.bound=*address;return 0; }
int SockSendTo(int,const void* buffer,uint32_t length,const SockAddrIn* destination) {
    ++g_net.sends;g_net.sentLength=length;
    __builtin_memcpy(g_net.sent,buffer,length<sizeof(g_net.sent)?length:sizeof(g_net.sent));
    g_net.sentWithDestination=destination!=nullptr;if(destination)g_net.sentDestination=*destination;
    return static_cast<int>(length);
}
int SockSendUser(int,uint64_t,uint32_t) { return -EIO; }
int SockConnect(int,const SockAddrIn*) { return -ECONNREFUSED; }
int SockGetType(int) { return 2; }
int SockListen(int,int) { return -1; }
int SockRecvFrom(int,void* buffer,uint32_t length,SockAddrIn* source) {
    if(!g_net.datagramReady)return -EAGAIN;
    ++g_net.receives;g_net.datagramReady=false;
    const uint32_t n=g_net.datagramLength<length?g_net.datagramLength:length;
    __builtin_memcpy(buffer,g_net.datagram,n);if(source)*source=g_net.source;
    return static_cast<int>(n);
}
int SockRecv(int,void*,uint32_t,bool) { return -EAGAIN; }
void SockGetLocal(int,uint32_t* ip,uint16_t* port) { *ip=g_net.localIp;*port=g_net.localPort; }
void SockGetRemote(int,uint32_t* ip,uint16_t* port) { *ip=g_net.remoteIp;*port=g_net.remotePort; }
int SockAccept(int,SockAddrIn* address) {
    if(!g_net.acceptReady)return -EAGAIN;
    ++g_net.accepts;g_net.acceptReady=false;*address=g_net.peer;return 5;
}
void SockSetAcceptedOwner(int,uint32_t,uint32_t) {}
void SockSetNonblock(int,bool) {}
void SockClearPollWaiter(int,Process*) {}
void SockClose(int) {}
void SockUnref(int) {}
bool SockGetIdentity(int,SocketIdentity* identity) { identity->inode=77;return true; }
}
extern "C" bool InputPollEvent(brook::InputEvent*) { return false; }
extern "C" int VfsRead(brook::Vnode*,void*,uint64_t,uint64_t*) { return -brook::EIO; }
extern "C" void VfsClose(brook::Vnode*) {}
