// Minimal stand-ins for subsystems underneath the handlers under test. Most
// fail or report "absent"; the fake network stack is observable via g_net.
#include "harness.h"
#include "syscall_internal.h"
#include "tcp_info.h"
#include "syscall_drm.h"
#include "syscall_memfd.h"
#include "syscall_pipe.h"
#include "syscall_poll.h"
#include "syscall_epoll.h"
#include "audio.h"
#include "ahci_system_volume.h"
#include "hda_observe.h"
#include "intel_bdw_ioctl.h"
#include "terminal.h"
#include "compositor.h"
#include "sync/ksemaphore.h"
#include "posix_timer.h"
#include "kvmclock.h"
#include "tsc_clock.h"
#include "rtc.h"
#include "syscall_dsp.h"
#include "dsp_producer_timing.h"
#include "serial_writer.h"
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
bool g_syntheticPathsOnly;
const brook::GpuAppOps* g_gpuApp;
}
using harness::g_net;

namespace brook {
// Filesystem: every lookup fails, so open stops at the first one, unless a
// test opts in to brokered synthetic paths (/dev/tty and friends).
int ProcessFsSnapshot(Process*,FsSnapshot*) { ++harness::g_snapshots;return harness::g_syntheticPathsOnly?0:-ENOENT; }
void FsSnapshotRelease(FsSnapshot*) {}
void VfsPathRetain(const VfsPath&) {}
void VfsPathRelease(VfsPath) {}
bool VfsPathSame(const VfsPath&,const VfsPath&) { return false; }
bool VfsPathSameObject(const VfsPath&,const VfsPath&) { return false; }
int VfsPathBrokerAllowed(const VfsPath&,const char*,bool* allowed) {
    if(!harness::g_syntheticPathsOnly)return -ENOENT;
    *allowed=true;return 0;
}
int VfsCanonicalDeviceAllowed(const VfsPath&,const VfsPath&,const char*,bool*,bool,const char*) { return -ENOENT; }
int VfsCanonicalDrmBrokerAllowed(const VfsPath&,bool,bool*,bool) { return -ENOENT; }
int VfsPathDeviceActivation(const VfsPath&,const char*) { return harness::g_syntheticPathsOnly?0:-ENOENT; }
int VfsPathFromVnode(Vnode*,VfsPath*) { return -ENOENT; }
int VfsPathResolve(const VfsPath&,const VfsPath&,const char*,VfsPath*,bool,bool) { return -ENOENT; }
int VfsPathDirectoryPath(const VfsPath&,char*,uint32_t) { return -ENOENT; }
Vnode* VfsOpenAt(const VfsPath&,const VfsPath&,const char*,int,uint16_t,uint16_t,int* error,bool) { if(error)*error=-ENOENT;return nullptr; }
Vnode* VfsCreateSocketAt(const VfsPath&,const VfsPath&,const char*,uint16_t,uint16_t,int* error) { if(error)*error=-ENOENT;return nullptr; }
void VfsRelease(Vnode*) {}
int VfsStat(Vnode*,VnodeStat*) { return -ENOENT; }
Vnode* GuardCheckerOpenLinux(uint64_t,int* error) { *error=-13;return nullptr; }

// Devices and GPU: absent.
const GpuAppOps* GpuAppGet() { return harness::g_gpuApp; }
bool BdwIntelUserNodeEnabled() { return false; }
BdwGpuFile* BdwGpuFileCreate() { return nullptr; }
void BdwGpuFileUnref(BdwGpuFile*) {}
void BdwGpuBufferUnref(BdwGpuBuffer*) {}
void BdwGpuBufferTrackUserRef(BdwGpuBuffer*,BdwBufferUserRef,bool) {}
void BdwGpuSyncUnref(BdwGpuSync*) {}
void SceneHandleUnref(SceneHandle*) {}
void SignalFdUnref(SignalFd*) {}
void MappingBackingUnref(MappingBacking*) {}
void FinalizeClosedFd(const FdClaimResult&) {}
// Real publication of a new open file description (process.cpp mirror).
int FdPublishNew(Process* proc,const FdEntry& owned,uint32_t openFlags,bool pinned,
                 void (*published)(int,const FdEntry&)) {
    OpenFile* file=OpenFileCreate(openFlags);if(!file)return -12;
    FdEntry entry=owned;entry.file=file;
    int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),entry,0,-1,nullptr,published,nullptr,nullptr,pinned);
    if(fd<0)OpenFileRelease(file);
    return fd;
}
int FdPublishPrepared(Process* proc,const FdEntry& owned,OpenFile* file,
                      void (*published)(int,const FdEntry&)) {
    FdEntry entry=owned;entry.file=file;
    return FdTablePublishOwned(proc->fds,ProcessFdLock(proc),entry,0,-1,nullptr,published);
}
int FdPublishNewPair(Process* proc,const FdEntry& first,uint32_t firstFlags,const FdEntry& second,
                     uint32_t secondFlags,int out[2],void (*published)(int,const FdEntry&)) {
    OpenFile* a=OpenFileCreate(firstFlags);OpenFile* b=a?OpenFileCreate(secondFlags):nullptr;
    if(!b){OpenFileRelease(a);return -12;}
    FdEntry entries[2]={first,second};entries[0].file=a;entries[1].file=b;
    int result=FdTablePublishPair(proc->fds,ProcessFdLock(proc),entries[0],entries[1],out,published);
    if(result<0){OpenFileRelease(a);OpenFileRelease(b);}
    return result;
}
int FdAlloc(Process* proc,FdType type,void* handle,uint32_t openFlags,uint8_t flags,uint8_t fdFlags) {
    FdEntry owned{};owned.type=type;owned.handle=handle;owned.flags=flags;owned.fdFlags=fdFlags;
    return FdPublishNew(proc,owned,openFlags);
}
// dup()-style retain of the description; handle references are not modelled.
bool FdRetainCopy(FdEntry* copy) { OpenFileRetain(copy->file);return true; }
AudioStreamId AudioMixerOpen() { return 0; }
bool AudioMixerClose(AudioStreamId) { return true; }
bool AudioMixerSetCapacity(AudioStreamId,uint32_t,uint32_t) { return false; }
bool AudioAvailable() { return false; }
uint32_t DebugOverlayReadFrom(uint32_t*,char*,uint32_t,uint32_t) { return 0; }
void InputAddWaiter(Process*) {}
void InputRemoveWaiter(Process*) {}
void TtyPutChar(char) {}
bool WmIsActive() { return true; }
int64_t SignalFdRead(Process*,SignalFd*,uint64_t,uint64_t,bool) { return -EBADF; }
uint64_t TimerFdDeadline(TimerFdData*,uint64_t*) { return 0; }
uint64_t SockNetworkScope(int) { return 0; }
uint32_t NetInterfaceViews(InterfaceView*,uint32_t,uint64_t) { return 0; }
FileActorSnapshot ProcessFileActorAcquire(Process*,UserNamespace*,bool) { return {}; }
void ProcessGroupsRelease(SupplementaryGroups*) {}
CredentialIds ProcessReadIds(Process*,bool) { return {}; }
UserAuthority ProcessUserAuthorityAcquire(Process*) { return {}; }
int ProcessSendSignal(Process*,int,const SignalInfo*,const Process*) { return 0; }
int VfsAccess(Vnode*,uint8_t,bool) { return -13; }
uint32_t ProcessReportId(Process*,bool,uint32_t canonical) { return canonical; }

bool AudioMixerGetProgress(AudioStreamId,AudioStreamProgress*) { return false; }
int EpollPollReady(Process*,EpollInstance*) { return 0; }
FdEntry* FdGet(Process* proc,int fd) { return fd>=0 && fd<static_cast<int>(MAX_FDS) && proc->fds[fd].type!=FdType::None?&proc->fds[fd]:nullptr; }
uint64_t NetTcpPoolWaitRegister(Process*) { return 0; }
void NetTcpPoolWaitUnregister(uint64_t) {}
uint32_t SignalFdReady(Process*,SignalFd*,uint32_t) { return 0; }
bool SockPollReady(int,bool,bool) { return false; }
bool SockPollHangup(int) { return false; }
void SockSetPollWaiter(int,Process*) {}
bool TimerFdReady(TimerFdData*) { return false; }

// write() backends not exercised by these tests.
uint32_t AudioMixerSubmit(const int16_t*,uint32_t,AudioStreamId) { return 0; }
uint32_t AudioMixerAvailableFrames(AudioStreamId) { return 0; }
uint64_t DspProducerTimingToken() { return 0; }
void DspProducerTimingRecord(uint64_t,const DspProducerEvent&) {}
bool DspRecoverStreamLocked(DspState*,AudioStreamProgress&) { return false; }
Process* SchedulerCurrentProcess() { return ProcessCurrent(); }
void SerialWriterEnqueue(const char*,uint32_t) {}
int SockSend(int,const void*,uint32_t) { return -EIO; }
int VfsFsync(Vnode*) { return 0; }

// Clocks and system counters (for syscall_time.cpp / syscall_sysinfo.cpp).
bool KvmClockAvailable() { return false; }
uint64_t KvmClockReadNs() { return 0; }
bool TscClockAvailable() { return false; }
uint64_t TscClockReadNs() { return 0; }
uint64_t RtcNowNs() { return 0; }
int PosixTimerGet(Process*,int32_t,PosixItimerspec*) { return -EINVAL; }
uint64_t PmmGetFreePageCount() { return 1; }
uint64_t PmmGetRamPageCount() { return 2; }
void SchedulerGetLoadAvg(uint32_t& a,uint32_t& b,uint32_t& c) { a=b=c=0; }
void SchedulerGetProcessCounts(uint32_t& total,uint32_t& running) { total=running=1; }
void SchedulerGetReapedTicks(uint64_t& user,uint64_t& system) { user=system=0; }

// ioctl/DRM neighbours: devices, audio, windows and memory not under test.
bool AhciSystemVolumeOwnsMember(Vnode*) { return false; }
uint32_t AudioGetPosition() { return 0; }
bool AudioMixerHorizonEnabled() { return false; }
void AudioMixerRefresh() {}
bool AudioMixerSetEnabled(AudioStreamId,bool) { return false; }
bool AudioMixerSetWritableFrames(AudioStreamId,uint32_t) { return false; }
int64_t BdwIntelRenderIoctl(BdwGpuFile*,uint64_t,uint64_t,const BdwI915UserAccess&) { return -EINVAL; }
int64_t CloseProcessFd(Process*,int) { return 0; }
bool CompositorResizeVfb(Process*,uint32_t,uint32_t) { return false; }
bool CompositorSetupProcess(Process*,int16_t,int16_t,uint32_t,uint32_t,uint8_t) { return false; }
int DspChangeFormat(DspState*,const AudioStreamProgress&,uint32_t,uint8_t,uint8_t) { return -EINVAL; }
bool DspResetStreamLocked(DspState*) { return false; }
bool HdaRuntimeDrainClockAvailable() { return false; }
uint64_t HdaRuntimeDrainClockNs() { return 0; }
Process* KernelThreadCreate(const char*,KernelThreadFn,void*,uint8_t) { return nullptr; }
void KSemaphoreSignal(KSemaphore*) {}
void KSemaphoreWait(KSemaphore*) {}
MappingBacking* MappingBackingCreate(void*,void (*)(void*)) { return nullptr; }
void MappingBackingRef(MappingBacking*) {}
int64_t sys_mmap(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) { return -ENOMEM; }
Terminal* TerminalFindByProcess(Process*) { return nullptr; }
bool TerminalSnapshotGeometry(Process*,TerminalGeometry* geometry) { geometry->rows=24;geometry->cols=80;return true; }
VirtualAddress VmmAllocPages(uint64_t,uint64_t,MemTag,uint16_t) { return VirtualAddress(0); }
void VmmFreePages(VirtualAddress,uint64_t) {}
int WmCreateWindow(Process*,int16_t,int16_t,uint16_t,uint16_t,const char*,uint8_t,bool,bool,bool) { return -1; }
int WmFindWindowForProcess(Process*) { return -1; }
uint32_t WmMetric(uint32_t base) { return base; }
const DesktopMetrics& WmDesktopMetrics() { return DesktopMetric100; } // consistent with WmMetric
bool WmResizeWindow(int,uint16_t,uint16_t) { return false; }

// Inet sockets: one fake UDP/TCP socket, observable through g_net. The
// stack only ever receives kernel buffers.
bool SockIsNonblock(int) { return true; }
bool SockIsStream(int) { return false; }
int SockBind(int,const SockAddrIn* address,SocketBindActor) { ++g_net.binds;g_net.bound=*address;return 0; }
int SockSendTo(int,const void* buffer,uint32_t length,const SockAddrIn* destination) {
    ++g_net.sends;g_net.sentLength=length;
    __builtin_memcpy(g_net.sent,buffer,length<sizeof(g_net.sent)?length:sizeof(g_net.sent));
    g_net.sentWithDestination=destination!=nullptr;if(destination)g_net.sentDestination=*destination;
    return static_cast<int>(length);
}
int SockSendUser(int,uint64_t,uint32_t) { return -EIO; }
int SockConnect(int,const SockAddrIn*) { return -ECONNREFUSED; }
int SockGetType(int) { return 2; }
int SockSetReceiveBuffer(int,int32_t value) { g_net.receiveBufferRequest=value;return 0; }
int SockGetReceiveBuffer(int) { return g_net.receiveBuffer; }
int SockGetProtocol(int) { return 17; }
int SockGetConnectError(int) { return 0; }
uint32_t SmpGetCpuCount() { return 4; }
int SockListen(int,int,SocketBindActor) { return -1; }
int SockSetReuse(int,SocketReuse,bool) { return 0; }
int SockGetReuse(int,SocketReuse) { return 0; }
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
int SockAccept(int,SockAddrIn* address,bool*) {
    if(!g_net.acceptReady)return -EAGAIN;
    ++g_net.accepts;g_net.acceptReady=false;*address=g_net.peer;return 5;
}
void SockSetAcceptedOwner(int,uint32_t,uint32_t) {}
void SockSetNonblock(int,bool) {}
void SockClearPollWaiter(int,Process*) {}
void SockClose(int) {}
void SockUnref(int) {}
bool SockGetIdentity(int,SocketIdentity* identity) { identity->inode=77;return true; }
bool SockGetTcpInfo(int,TcpInfoSource* out) {
    if(!g_net.tcp)return false;
    *out={};out->brookState=g_net.tcpState;out->peerMss=g_net.peerMss;out->advertisedMss=1460;out->mtu=1500;
    out->sndIss=1000;out->sndUna=1001+5000;out->sndNxt=1001+5000+2800;out->peerWindow=65535;
    out->receiveFree=4096;out->segmentsIn=7;out->segmentsOut=9;out->delayedAckUs=40000;
    out->nowMs=5000;out->lastReceiveMs=4750;out->sndWscale=7;
    return true;
}
}
extern "C" bool InputPollEvent(brook::InputEvent*) { return false; }
extern "C" bool InputHasEvents() { return false; }
extern "C" uint64_t AudioClockNowNs() { return 0; }
extern "C" void CompositorWake() {}
extern "C" void KPuts(const char*) {}
extern "C" void KPrintf(const char*,...) {}
extern "C" bool TtyGetFramebufferPhys(uint64_t*,uint32_t*,uint32_t*,uint32_t*) { return false; }
static harness::MemoryFile* FakeFile(brook::Vnode* vn) {
    return vn ? static_cast<harness::MemoryFile*>(vn->priv) : nullptr;
}
extern "C" int VfsWrite(brook::Vnode* vn,const void* buf,uint64_t len,uint64_t* offset) {
    auto* file=FakeFile(vn);if(!file)return -brook::EIO;
    if(*offset>sizeof(file->bytes) || len>sizeof(file->bytes)-*offset)return -28; // ENOSPC
    __builtin_memcpy(file->bytes+*offset,buf,len);*offset+=len;
    if(*offset>file->size)file->size=*offset;
    return static_cast<int>(len);
}
extern "C" int VfsAppend(brook::Vnode* vn,const void* buf,uint64_t len,uint64_t* offset) {
    auto* file=FakeFile(vn);if(!file)return -brook::EIO;
    *offset=file->size;return VfsWrite(vn,buf,len,offset);
}
extern "C" int VfsRead(brook::Vnode* vn,void* buf,uint64_t len,uint64_t* offset) {
    auto* file=FakeFile(vn);if(!file)return -brook::EIO;
    if(*offset>=file->size)return 0;
    if(len>file->size-*offset)len=file->size-*offset;
    __builtin_memcpy(buf,file->bytes+*offset,len);*offset+=len;
    return static_cast<int>(len);
}
extern "C" void VfsClose(brook::Vnode*) {}
