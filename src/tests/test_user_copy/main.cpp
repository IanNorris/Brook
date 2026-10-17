#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstdint>
#include <initializer_list>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ucontext.h>
#include "user_copy_fault.h"
#include "signal_return.h"
#define CHECK(c) do { if(!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);std::abort(); } } while(0)
using namespace brook;
extern "C" const char BrookCopyFromUserInstruction[],BrookCopyToUserInstruction[];
static volatile sig_atomic_t handled;
static volatile uint64_t editR11;
static volatile sig_atomic_t invalidContext;
extern "C" uint64_t CopyFromWithR11(void*,const void*,uint64_t,uint64_t*);
asm(".text\n.global CopyFromWithR11\n.type CopyFromWithR11,@function\n"
    "CopyFromWithR11:\n push %r12\n mov %rcx,%r12\n"
    "movabs $0x1122334455667788,%r11\n call BrookCopyFromUserRaw\n"
    "mov %r11,(%r12)\n pop %r12\n ret\n"
    ".size CopyFromWithR11,.-CopyFromWithR11\n");
static void recover(int,siginfo_t* info,void* context) {
    auto& registers=static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    // Real assembly faults, translated from Linux's CPL3 signal frame into
    // the CPL0 #PF framing used by Brook. No copied byte/count is fabricated.
    UserCopyFault fault{static_cast<uint64_t>(registers[REG_RIP]),reinterpret_cast<uint64_t>(info->si_addr),
        static_cast<uint64_t>(registers[REG_RSI]),static_cast<uint64_t>(registers[REG_RDI]),
        static_cast<uint64_t>(registers[REG_RCX]),static_cast<uint64_t>(registers[REG_ERR])&~uint64_t{4},0,
        static_cast<uint64_t>(registers[REG_EFL])};
    const auto fixup=UserCopyFaultFixup(fault);
    if(!fixup)_exit(90);
    if(editR11)registers[REG_R11]=static_cast<greg_t>(editR11);
    ++handled;registers[REG_RIP]=static_cast<greg_t>(fixup);
    if(invalidContext) {
        struct sigaction reset{};reset.sa_handler=SIG_DFL;
        if(sigaction(SIGSEGV,&reset,nullptr))_exit(92);
        if(invalidContext==1)registers[REG_RIP]=static_cast<greg_t>(0x1000000000000001ULL);
        else registers[REG_RSP]=static_cast<greg_t>(0xffff800000000000ULL);
    }
}
int main() {
    struct sigaction action{};action.sa_sigaction=recover;action.sa_flags=SA_SIGINFO;
    CHECK(!sigaction(SIGSEGV,&action,nullptr));
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));CHECK(page==4096);
    auto* memory=static_cast<unsigned char*>(mmap(nullptr,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    CHECK(memory!=MAP_FAILED);std::memset(memory,0x5a,page);CHECK(!mprotect(memory+page,page,PROT_NONE));
    unsigned char buffer[128];std::memset(buffer,0xa3,sizeof(buffer));
    CHECK(!BrookCopyFromUserRaw(buffer,memory,64)&&!handled);
    CHECK(!BrookCopyFromUserRaw(nullptr,nullptr,0)&&!BrookCopyToUserRaw(nullptr,nullptr,0));
    std::memset(buffer,0xa3,sizeof(buffer));
    CHECK(BrookCopyFromUserRaw(buffer,memory+page-31,64)==33&&handled==1);
    for(unsigned i=0;i<sizeof(buffer);++i)CHECK(buffer[i]==(i<31?0x5a:0xa3));
    std::memset(buffer,0x72,sizeof(buffer));
    CHECK(BrookCopyToUserRaw(memory+page-31,buffer,64)==33&&handled==2);
    for(unsigned i=0;i<31;++i)CHECK(memory[page-31+i]==0x72);
    CHECK(!mprotect(memory,page,PROT_READ));
    CHECK(BrookCopyToUserRaw(memory,buffer,16)==16&&handled==3&&memory[0]==0x5a);
    // rt_sigreturn must preserve arbitrary RCX/R11, not replace them with
    // SYSRET's RIP/RFLAGS. RCX is checked through the REP residual return.
    for(unsigned edit=0;edit<2;++edit) {
        uint64_t r11=0;editR11=edit?0x8877665544332211ULL:0;
        CHECK(CopyFromWithR11(buffer,memory+page-31,64,&r11)==33);
        CHECK(r11==(edit?0x8877665544332211ULL:0x1122334455667788ULL));
    }
    editR11=0;CHECK(handled==5);
    UserCopyFault good{reinterpret_cast<uint64_t>(BrookCopyFromUserInstruction),0x10000,0x10000,0xffff800000010000ULL,64,0,0,512};
    CHECK(UserCopyFaultFixup(good));
    for(unsigned which=0;which<10;++which) {
        auto bad=good;
        switch(which) {
            case 0:++bad.instruction;break;
            case 1:++bad.address;break;
            case 2:bad.error|=2;break;
            case 3:bad.error|=4;break;
            case 4:bad.error|=8;break;
            case 5:bad.error|=16;break;
            case 6:bad.cs=3;break;
            case 7:bad.flags|=1024;break;
            case 8:bad.remaining=0;break;
            case 9:bad.remaining=~uint64_t{0};break;
        }
        CHECK(!UserCopyFaultFixup(bad));
    }
    auto to=good;to.instruction=reinterpret_cast<uint64_t>(BrookCopyToUserInstruction);
    to.destination=to.address;to.source=0xffff800000010000ULL;to.error=2;CHECK(UserCopyFaultFixup(to));
    to.destination=to.address=0x0000800000000000ULL;CHECK(!UserCopyFaultFixup(to));
    SyscallReturnState state{};
    CHECK(UserReturnAddress(0x10000)&&UserReturnAddress(0x7fffffffffff));
    CHECK(!UserReturnAddress(0xffff)&&!UserReturnAddress(0x800000000000));
    CHECK(!UserReturnAddress(0x1000000000000001)&&!UserReturnAddress(0xffff800000000000));
    CHECK(PrepareSignalReturn(state,0x401000,0x7fffffffe000,~uint64_t{0},0x123456));
    CHECK(state.mode==1&&state.rcx==0x123456&&state.rip==0x401000&&state.rsp==0x7fffffffe000);
    CHECK(state.cs==GDT_USER_CODE&&state.ss==GDT_USER_DATA&&state.flags==0x250fd7&&state.padding==0);
    CHECK(state.cs==0x2b&&state.ss==0x23);
    for(uint64_t address:{uint64_t{0},uint64_t{0x800000000000},uint64_t{0x1000000000000001},~uint64_t{0}}) {
        CHECK(!PrepareSignalReturn(state,address,0x7fffffffe000,0,0));
        CHECK(!PrepareSignalReturn(state,0x401000,address,0,0));
        CHECK(state.rcx==0x123456&&state.rip==0x401000);
    }
    // An inaccessible TRUSTED side must not be mistaken for a user fault.
    for(unsigned direction=0;direction<2;++direction) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {
            if(direction)BrookCopyToUserRaw(buffer,memory+page,16);
            else BrookCopyFromUserRaw(memory+page,buffer,16);
            _exit(91);
        }
        int status=0;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==90);
    }
    // A malformed signal return is user-fatal, never a CPL0 IRET #GP/panic.
    for(unsigned which=1;which<=2;++which) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) { invalidContext=which;BrookCopyFromUserRaw(buffer,memory+page-31,64);_exit(91); }
        int status=0;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
    }
    CHECK(!munmap(memory,2*page));
    std::puts("USER COPY PASS actual-REP-faults/partial-counts/RCX-R11-signal-return/bad-context-user-fatal/zero-length/readonly/trusted-side-rejection/exact-RIP/address/direction/CPL/flags/overflow");
}
