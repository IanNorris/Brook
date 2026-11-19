#include <cassert>
#include <cstdio>
#include <cstring>
#include "../../kernel/src/proc_wait_info.h"
using namespace brook;
int main(){
 // Not waiting.
 assert(!strcmp(ProcWaitKind(false,false,false,202,-1,false,nullptr),"running"));
 assert(!strcmp(ProcWaitKind(true,true,false,202,-1,false,nullptr),"stopped"));
 // Syscall-number kinds.
 assert(!strcmp(ProcWaitKind(true,false,false,202,-1,false,"FutexWaitCommon+0x80"),"futex"));
 assert(!strcmp(ProcWaitKind(true,false,false,7,-1,false,nullptr),"poll"));
 assert(!strcmp(ProcWaitKind(true,false,false,271,-1,false,nullptr),"poll"));
 assert(!strcmp(ProcWaitKind(true,false,false,281,-1,false,nullptr),"epoll"));
 assert(!strcmp(ProcWaitKind(true,false,false,230,-1,false,nullptr),"sleep"));
 assert(!strcmp(ProcWaitKind(true,false,false,61,-1,false,nullptr),"wait-child"));
 // fd-typed I/O.
 assert(!strcmp(ProcWaitKind(true,false,false,0,ProcFd::Pipe,false,nullptr),"pipe"));
 assert(!strcmp(ProcWaitKind(true,false,false,47,ProcFd::UnixSocket,false,nullptr),"unix-socket"));
 assert(!strcmp(ProcWaitKind(true,false,false,0,ProcFd::EventFd,false,nullptr),"eventfd"));
 assert(!strcmp(ProcWaitKind(true,false,false,16,ProcFd::DevIntelDri,false,nullptr),"gpu-wait"));
 assert(!strcmp(ProcWaitKind(true,false,false,16,ProcFd::Vnode,false,nullptr),"ioctl"));
 assert(!strcmp(ProcWaitKind(true,false,false,0,ProcFd::Vnode,false,nullptr),"file-io"));
 assert(!strcmp(ProcWaitKind(true,false,false,1,-1,false,nullptr),"io"));
 // Lock and GPU wait sites override the syscall.
 assert(!strcmp(ProcWaitKind(true,false,false,202,-1,true,nullptr),"rwlock"));
 assert(!strcmp(ProcWaitKind(true,false,false,202,-1,false,"KMutexLock+0x40"),"kmutex"));
 assert(!strcmp(ProcWaitKind(true,false,false,16,ProcFd::Vnode,false,"IntelBdwWaitQueueWait+0x10"),"gpu-wait"));
 // ... but a futex/poll/epoll wait reached through a GPU helper stays what it is.
 assert(!strcmp(ProcWaitKind(true,false,false,202,-1,false,"GpuFenceHelper+0x10"),"futex"));
 assert(!strcmp(ProcWaitKind(true,false,true,0,-1,false,"WorkerLoop+0x10"),"kernel"));
 assert(!strcmp(ProcWaitKind(true,false,false,510,-1,false,nullptr),"brook-wm"));
 assert(!strcmp(ProcWaitKind(true,false,false,0xB10,-1,false,nullptr),"brook-ext"));
 assert(ProcWaitSyscallUsesFd(0)&&ProcWaitSyscallUsesFd(16)&&!ProcWaitSyscallUsesFd(202));
 // Symbols.
 char sym[16];
 ProcWaitShortSymbol("brook::FutexWait(int, unsigned long)",sym,sizeof(sym));assert(!strcmp(sym,"FutexWait"));
 ProcWaitShortSymbol("brook::VeryLongFunctionNameHere()",sym,sizeof(sym));assert(strlen(sym)==15);
 ProcWaitShortSymbol("SchedulerYield",sym,sizeof(sym));assert(!strcmp(sym,"SchedulerYield"));
 // /proc/<pid>/syscall formatting.
 char line[160];const uint64_t args[6]={3,0x7f00,0x80,0,0,0xffffffffffffffffULL};
 ProcFormatSyscall(line,sizeof(line),false,202,args,0x7ffd1000,0x401234);
 assert(!strcmp(line,"202 0x3 0x7f00 0x80 0x0 0x0 0xffffffffffffffff 0x7ffd1000 0x401234\n"));
 ProcFormatSyscall(line,sizeof(line),true,202,args,0,0);assert(!strcmp(line,"running\n"));
 char tiny[8];const uint32_t n=ProcFormatSyscall(tiny,sizeof(tiny),false,202,args,1,2);
 assert(n==7&&tiny[7]==0);
 puts("proc wait info: kinds, fd kinds, lock/gpu sites, symbols, syscall lines passed");
}
