// H-17 getdents64 and H-18 open/openat: user pointers only, checked copies.
#include "harness.h"
#include "syscall_open.cpp"
#include "syscall_dir.cpp"

using namespace harness;
namespace {
const char* g_names[]={"alpha","beta","gamma"};
}

extern "C" int VfsReaddir(brook::Vnode*,brook::DirEntry* out,uint32_t* cookie) {
    if(*cookie>=3)return 0;
    const char* name=g_names[*cookie];
    uint32_t i=0;for(;name[i];++i)out->name[i]=name[i];out->name[i]=0;
    out->isDir=false;++*cookie;return 1;
}

static void Open() {
    Reset();ResetProcess();g_snapshots=0;
    // Kernel-half and kernel-looking host pointers are both refused before
    // any lookup reads them.
    CHECK(brook::sys_openat(static_cast<uint64_t>(-100),0xffff800000000000ULL,0,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_open(0xffffffff80001000ULL,0,0,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_open(g_kernel.Address(),0,0,0,0,0)==-brook::EFAULT);
    CHECK(g_snapshots==0);
    // A user path reaches the (failing) lookup.
    __builtin_memcpy(reinterpret_cast<void*>(UserAddress(0)),"/tmp/x",7);
    CHECK(brook::sys_open(UserAddress(0),0,0,0,0,0)==-brook::ENOENT);
    CHECK(g_snapshots==1);
}

static void Getdents() {
    Reset();ResetProcess();
    auto* proc=CurrentProcess();
    static int node;
    proc->fds[3].type=brook::FdType::Vnode;proc->fds[3].handle=&node;
    // Kernel or unmapped destinations fail without writing or advancing.
    CHECK(brook::sys_getdents64(3,g_kernel.Address(),4096,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(proc->fds[3].seekPos==0);
    Protect(0,PageSize,Access::None);
    CHECK(brook::sys_getdents64(3,UserAddress(0),4096,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_getdents64(3,0,4096,0,0,0)==-brook::EFAULT);
    CHECK(proc->fds[3].seekPos==0);
    // A buffer that straddles into an unmapped page returns the records that
    // fit before it and resumes at the next one.
    const uint64_t edge=UserAddress(2*PageSize-32);
    Protect(2*PageSize,PageSize,Access::None);
    CHECK(brook::sys_getdents64(3,edge,4096,0,0,0)==32);
    CHECK(proc->fds[3].seekPos==1);
    CHECK(brook::StrEq(reinterpret_cast<const char*>(edge+19),"alpha"));
    // A good buffer gets the rest.
    const uint64_t good=UserAddress(4*PageSize);
    CHECK(brook::sys_getdents64(3,good,4096,0,0,0)==56);
    CHECK(brook::StrEq(reinterpret_cast<const char*>(good+19),"beta"));
    CHECK(brook::StrEq(reinterpret_cast<const char*>(good+24+19),"gamma"));
    CHECK(proc->fds[3].seekPos==3);
    // The synthetic /dev/dri directory follows the same rules.
    proc->fds[4].type=brook::FdType::SyntheticMem;proc->fds[4].handle=const_cast<char*>(brook::g_drmDriDirTag);
    CHECK(brook::sys_getdents64(4,g_kernel.Address(),4096,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(brook::sys_getdents64(4,good,4096,0,0,0)==48);
    CHECK(brook::StrEq(reinterpret_cast<const char*>(good+24+19),".."));
}

// H-27: /dev/tty borrows the stdin/stdout pipes only while they are pinned,
// retains both ends before publishing, and honours O_CLOEXEC.
static void DevTty() {
    Reset();ResetProcess();g_syntheticPathsOnly=true;
    auto* proc=CurrentProcess();
    auto* in=brook::PipeBufferCreate(64);auto* out=brook::PipeBufferCreate(64);
    in->readers=in->writers=1;out->readers=out->writers=1;
    proc->fds[0].type=brook::FdType::Pipe;proc->fds[0].handle=in;proc->fds[0].flags=1;
    proc->fds[1].type=brook::FdType::Pipe;proc->fds[1].handle=out;proc->fds[1].flags=1;
    __builtin_memcpy(reinterpret_cast<void*>(UserAddress(0)),"/dev/tty",9);
    const int64_t fd=brook::sys_openat(static_cast<uint64_t>(-100),UserAddress(0),2|0x80000,0,0,0);
    CHECK(fd==2);
    if(fd==2) {
        CHECK(proc->fds[2].type==brook::FdType::DevTty);
        CHECK(proc->fds[2].statusFlags==2 && proc->fds[2].fdFlags==1);
        auto* pair=static_cast<brook::TtyDevicePair*>(proc->fds[2].handle);
        CHECK(pair->readPipe==in && pair->writePipe==out);
    }
    CHECK(in->readers==2 && out->writers==2);
    CHECK(proc->fds[0].pinCount==0 && proc->fds[1].pinCount==0);
    // A stdin slot that a sibling is closing is not borrowed.
    proc->fds[0].closing=1;
    CHECK(brook::sys_openat(static_cast<uint64_t>(-100),UserAddress(0),2,0,0,0)==-brook::EMFILE);
    CHECK(in->readers==2);
    g_syntheticPathsOnly=false;
}

void RunDirOpenTests() {
    DevTty();
    Open();
    Getdents();
}
