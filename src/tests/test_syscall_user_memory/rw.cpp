// sys_read device branches (H-13, M-27): no store through an unchecked buffer.
#include "harness.h"
#include "syscall_rw.cpp"

using namespace harness;
namespace {
bool g_wmActive=true;
const char g_synthetic[]="root:x:0:0:root:/root:/bin/sh\n";
}

namespace brook {
uint64_t ClockNowNs(uint64_t) { return 0; }
uint32_t DebugOverlayReadFrom(uint32_t*,char*,uint32_t,uint32_t) { return 0; }
NamedFifoOpen* FdNamedFifo(const FdEntry*) { return nullptr; }
PipeBuffer* FdPipe(const FdEntry*) { return nullptr; }
bool FdPipeRead(const FdEntry*) { return false; }
void InputAddWaiter(Process*) {}
void InputRemoveWaiter(Process*) {}
int64_t MemFdReadAt(MemFdData*,void*,uint64_t,uint64_t*) { return -EBADF; }
int64_t NetlinkReadUser(NetlinkSocket*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) { return -EBADF; }
void PipePollNotify(PipeBuffer*,uint32_t) {}
int64_t SignalFdRead(Process*,SignalFd*,uint64_t,uint64_t,bool) { return -EBADF; }
bool SockIsNonblock(int) { return true; }
bool SockIsStream(int) { return false; }
int SockRecvFrom(int,void*,uint32_t,SockAddrIn*) { return -EAGAIN; }
int SockRecv(int,void*,uint32_t,bool) { return -EAGAIN; }
uint64_t TimerFdDeadline(TimerFdData*,uint64_t*) { return 0; }
void TtyPutChar(char) {}
void UnixFdSnapRelease(const UnixFdSnap*) {}
int64_t UnixSocketReadSome(UnixSocketData*,bool,char*,uint64_t,UnixFdSnap*,int&,UnixCredentials*,uint64_t*,bool*) { return -EBADF; }
uint64_t WaitWakeupTick(uint64_t,uint64_t) { return 0; }
bool WmIsActive() { return g_wmActive; }
}
extern "C" bool InputPollEvent(brook::InputEvent*) { return false; }
extern "C" int VfsRead(brook::Vnode*,void*,uint64_t,uint64_t*) { return -brook::EIO; }
bool RdrandU64(uint64_t* out) { *out=0x0807060504030201ULL;return true; }
uint64_t SoftRandU64() { return 0; }

static void PushKey(brook::Process* proc,uint8_t scan,char ascii,uint8_t modifiers=0) {
    brook::InputEvent event{};event.type=brook::InputEventType::KeyPress;
    event.scanCode=scan;event.ascii=ascii;event.modifiers=modifiers;
    brook::ProcessInputPush(proc,event);
}

static void CookedTtyCountZero() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    proc->ttyCanonical=true;
    proc->fds[0].type=brook::FdType::DevKeyboard;
    // Ctrl+C queued: a zero-length read into a kernel address writes nothing
    // and returns 0 rather than one byte.
    PushKey(proc,0x2E,'c',brook::INPUT_MOD_CTRL);
    CHECK(brook::sys_read(0,g_kernel.Address(),0,0,0,0)==0);
    CHECK(g_kernel.Intact());
    // With a real buffer the same event yields the newline.
    const uint64_t out=UserAddress(0);
    CHECK(brook::sys_read(0,out,16,0,0,0)==1);
    CHECK(*reinterpret_cast<const char*>(out)=='\n');
    // Kernel buffer with a count is refused up front.
    PushKey(proc,0x2E,'c',brook::INPUT_MOD_CTRL);
    CHECK(brook::sys_read(0,g_kernel.Address(),16,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    // ...without consuming the event.
    CHECK(brook::sys_read(0,out,16,0,0,0)==1);
    // A cooked line.
    PushKey(proc,0x23,'h');PushKey(proc,0x17,'i');PushKey(proc,0x1C,'\n');
    CHECK(brook::sys_read(0,out,16,0,0,0)==3);
    CHECK(__builtin_memcmp(reinterpret_cast<const void*>(out),"hi\n",3)==0);
}

static void RawKeyboardAndDevices() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    proc->fds[0].type=brook::FdType::DevKeyboard;proc->fds[0].flags=1;
    PushKey(proc,0x1E,'a');PushKey(proc,0x30,'b');
    CHECK(brook::sys_read(0,g_kernel.Address(),0,0,0,0)==0);
    const uint64_t out=UserAddress(0);
    CHECK(brook::sys_read(0,out,8,0,0,0)==2);
    CHECK(reinterpret_cast<const uint8_t*>(out)[1]==0x30);
    // Non-canonical mode.
    proc->fds[0].flags=0;proc->ttyCanonical=false;
    PushKey(proc,0x1E,'a');
    CHECK(brook::sys_read(0,out,8,0,0,0)==1);
    CHECK(*reinterpret_cast<const char*>(out)=='a');
    // /dev/urandom and synthetic files stage through kernel memory.
    proc->fds[1].type=brook::FdType::DevUrandom;
    CHECK(brook::sys_read(1,out,300,0,0,0)==300);
    CHECK(reinterpret_cast<const uint8_t*>(out)[299]==0x04);
    CHECK(brook::sys_read(1,g_kernel.Address(),16,0,0,0)==-brook::EFAULT);
    proc->fds[2].type=brook::FdType::SyntheticMem;proc->fds[2].handle=const_cast<char*>(g_synthetic);
    CHECK(brook::sys_read(2,out,4,0,0,0)==4);
    CHECK(__builtin_memcmp(reinterpret_cast<const void*>(out),"root",4)==0);
    CHECK(proc->fds[2].seekPos==4);
    CHECK(g_kernel.Intact());
}

void RunRwTests() {
    CookedTtyCountZero();
    RawKeyboardAndDevices();
}
