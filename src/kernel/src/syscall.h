#pragma once

#include <stdint.h>

namespace brook {

// Maximum number of syscalls. Must match the dispatcher bounds check.
static constexpr uint64_t SYSCALL_MAX = 400;

// Syscall numbers (Linux x86-64 ABI)
static constexpr uint64_t SYS_READ            = 0;
static constexpr uint64_t SYS_WRITE           = 1;
static constexpr uint64_t SYS_OPEN            = 2;
static constexpr uint64_t SYS_CLOSE           = 3;
static constexpr uint64_t SYS_FSTAT           = 5;
static constexpr uint64_t SYS_STAT            = 4;
static constexpr uint64_t SYS_LSTAT           = 6;
static constexpr uint64_t SYS_LSEEK           = 8;
static constexpr uint64_t SYS_MMAP            = 9;
static constexpr uint64_t SYS_MPROTECT        = 10;
static constexpr uint64_t SYS_MUNMAP          = 11;
static constexpr uint64_t SYS_BRK             = 12;
static constexpr uint64_t SYS_IOCTL           = 16;
static constexpr uint64_t SYS_RT_SIGACTION    = 13;
static constexpr uint64_t SYS_RT_SIGPROCMASK = 14;
static constexpr uint64_t SYS_PREAD64         = 17;
static constexpr uint64_t SYS_READV           = 19;
static constexpr uint64_t SYS_WRITEV          = 20;
static constexpr uint64_t SYS_ACCESS          = 21;
static constexpr uint64_t SYS_NANOSLEEP       = 35;
static constexpr uint64_t SYS_GETPID          = 39;
static constexpr uint64_t SYS_EXECVE          = 59;
static constexpr uint64_t SYS_EXIT            = 60;
static constexpr uint64_t SYS_UNAME           = 63;
static constexpr uint64_t SYS_FCNTL           = 72;
static constexpr uint64_t SYS_GETCWD          = 79;
static constexpr uint64_t SYS_GETTIMEOFDAY    = 96;
static constexpr uint64_t SYS_GETUID           = 102;
static constexpr uint64_t SYS_GETGID           = 104;
static constexpr uint64_t SYS_SETUID           = 105;
static constexpr uint64_t SYS_SETGID           = 106;
static constexpr uint64_t SYS_GETEUID          = 107;
static constexpr uint64_t SYS_GETEGID          = 108;
static constexpr uint64_t SYS_ARCH_PRCTL      = 158;
static constexpr uint64_t SYS_GETDENTS64      = 217;
static constexpr uint64_t SYS_SET_TID_ADDRESS = 218;
static constexpr uint64_t SYS_CLOCK_GETTIME   = 228;
static constexpr uint64_t SYS_CLOCK_NANOSLEEP = 230;
static constexpr uint64_t SYS_EXIT_GROUP      = 231;
static constexpr uint64_t SYS_OPENAT          = 257;
static constexpr uint64_t SYS_NEWFSTATAT      = 262;
static constexpr uint64_t SYS_PRLIMIT64       = 302;
static constexpr uint64_t SYS_GETRANDOM       = 318;

// Syscall function type -- same signature as Linux: returns int64_t,
// up to 6 arguments via rdi, rsi, rdx, r10->rcx, r8, r9.
using SyscallFn = int64_t(*)(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);

// Returns the address of the SYSCALL dispatcher (for LSTAR).
uint64_t SyscallGetEntryPoint();

// Initialise the syscall table and update KernelCpuEnv.syscallTable.
void SyscallTableInit();

// Get pointer to the syscall table (for storing in KernelCpuEnv).
SyscallFn* SyscallGetTable();

// Switch to user mode.
// Saves kernel state, builds IRETQ frame, SWAPGS, jumps to ring 3.
// When the user process calls sys_exit, control returns here.
void SwitchToUserMode(uint64_t userStack, uint64_t userEntry);

} // namespace brook
