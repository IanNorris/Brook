// sys_read device branches (H-13, M-27): no store through an unchecked buffer.
#include "harness.h"
#include "syscall_rw.cpp"

using namespace harness;
namespace {
const char g_synthetic[]="root:x:0:0:root:/root:/bin/sh\n";
}

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
    proc->fds[2].type=brook::FdType::SyntheticMem;proc->fds[2].handle=const_cast<char*>(g_synthetic);Describe(proc->fds[2],0);
    CHECK(brook::sys_read(2,out,4,0,0,0)==4);
    CHECK(__builtin_memcmp(reinterpret_cast<const void*>(out),"root",4)==0);
    CHECK(brook::OpenFileOffset(proc->fds[2].file)==4);
    CHECK(g_kernel.Intact());
}

// H-12: a write-only descriptor cannot read a regular file.
static void WriteOnlyRead() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    static brook::Vnode node{};
    proc->fds[3].type=brook::FdType::Vnode;proc->fds[3].handle=&node;Describe(proc->fds[3],1); // O_WRONLY
    const uint64_t out=UserAddress(0);
    CHECK(brook::sys_read(3,out,16,0,0,0)==-brook::EBADF);
    CHECK(brook::sys_pread64(3,out,16,0,0,0)==-brook::EBADF);
    // O_RDONLY and O_RDWR reach the filesystem (whose fake read fails).
    proc->fds[3].file->statusFlags=0;
    CHECK(brook::sys_read(3,out,16,0,0,0)==-brook::EIO);
    proc->fds[3].file->statusFlags=2;
    CHECK(brook::sys_pread64(3,out,16,0,0,0)==-brook::EIO);
}

// H-20: dup/fork aliases share one open file description (offset and status
// flags); separate opens of the same file do not.
static void SharedDescriptions() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    static MemoryFile file;file=MemoryFile{};file.vnode.priv=&file;
    auto slot=[&](int fd,brook::OpenFile* description) {
        proc->fds[fd].type=brook::FdType::Vnode;proc->fds[fd].handle=&file.vnode;
        proc->fds[fd].file=description;
    };
    // `cmd >f 2>&1`: fd 1 and its dup fd 2 append at one shared offset.
    auto* shared=brook::OpenFileCreate(1|0x40|0x200); // O_WRONLY|O_CREAT|O_TRUNC
    slot(1,shared);slot(2,shared);
    const uint64_t a=UserAddress(0),b=UserAddress(16);
    __builtin_memcpy(reinterpret_cast<void*>(a),"a",1);
    __builtin_memcpy(reinterpret_cast<void*>(b),"b",1);
    CHECK(brook::sys_write(1,a,1,0,0,0)==1);
    CHECK(brook::sys_write(2,b,1,0,0,0)==1);
    CHECK(file.size==2 && __builtin_memcmp(file.bytes,"ab",2)==0);
    CHECK(brook::OpenFileOffset(shared)==2);
    CHECK(brook::FdStatus(&proc->fds[2])==1); // call-only flags stripped
    // A second, independent open starts at 0 and overwrites (Linux).
    slot(3,brook::OpenFileCreate(1));
    __builtin_memcpy(reinterpret_cast<void*>(a),"X",1);
    CHECK(brook::sys_write(3,a,1,0,0,0)==1);
    CHECK(__builtin_memcmp(file.bytes,"Xb",2)==0 && brook::OpenFileOffset(shared)==2);
    // O_APPEND on a separate description always lands at EOF.
    slot(4,brook::OpenFileCreate(1|0x400));
    __builtin_memcpy(reinterpret_cast<void*>(a),"c",1);
    CHECK(brook::sys_write(4,a,1,0,0,0)==1);
    CHECK(file.size==3 && __builtin_memcmp(file.bytes,"Xbc",3)==0);
    // The shared writer continues after the appended byte only through its own offset.
    CHECK(brook::sys_write(2,b,1,0,0,0)==1);
    CHECK(__builtin_memcmp(file.bytes,"Xbb",3)==0 && brook::OpenFileOffset(shared)==3);
    // Readers sharing a description consume the file once between them.
    auto* reader=brook::OpenFileCreate(0);
    slot(5,reader);slot(6,reader);
    const uint64_t out=UserAddress(64);
    CHECK(brook::sys_read(5,out,1,0,0,0)==1 && *reinterpret_cast<const char*>(out)=='X');
    CHECK(brook::sys_read(6,out,2,0,0,0)==2 && __builtin_memcmp(reinterpret_cast<const void*>(out),"bb",2)==0);
    CHECK(brook::sys_read(5,out,1,0,0,0)==0);
    // pread does not move the shared offset.
    CHECK(brook::sys_pread64(6,out,1,0,0,0)==1 && brook::OpenFileOffset(reader)==3);
}

void RunRwTests() {
    SharedDescriptions();
    WriteOnlyRead();
    CookedTtyCountZero();
    RawKeyboardAndDevices();
}
