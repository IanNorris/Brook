#pragma once

#include <stdint.h>

namespace brook {

// Maximum number of syscalls. Must match the dispatcher bounds check.
static constexpr uint64_t SYSCALL_MAX = 512;

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
static constexpr uint64_t SYS_PWRITE64        = 18;
static constexpr uint64_t SYS_READV           = 19;
static constexpr uint64_t SYS_WRITEV          = 20;
static constexpr uint64_t SYS_ACCESS          = 21;
static constexpr uint64_t SYS_SCHED_YIELD     = 24;
static constexpr uint64_t SYS_MINCORE         = 27;
static constexpr uint64_t SYS_MADVISE         = 28;
static constexpr uint64_t SYS_NANOSLEEP       = 35;
static constexpr uint64_t SYS_GETPID          = 39;
static constexpr uint64_t SYS_PIPE            = 22;
static constexpr uint64_t SYS_DUP             = 32;
static constexpr uint64_t SYS_DUP2            = 33;
static constexpr uint64_t SYS_CLONE           = 56;
static constexpr uint64_t SYS_FORK            = 57;
static constexpr uint64_t SYS_VFORK           = 58;
static constexpr uint64_t SYS_EXECVE          = 59;
static constexpr uint64_t SYS_EXIT            = 60;
static constexpr uint64_t SYS_WAIT4           = 61;
static constexpr uint64_t SYS_UNAME           = 63;
static constexpr uint64_t SYS_FCNTL           = 72;
static constexpr uint64_t SYS_GETCWD          = 79;
static constexpr uint64_t SYS_GETTIMEOFDAY    = 96;
static constexpr uint64_t SYS_GETUID           = 102;
static constexpr uint64_t SYS_GETGID           = 104;
static constexpr uint64_t SYS_SETUID           = 105;
static constexpr uint64_t SYS_SETGID           = 106;

static constexpr uint64_t SYS_TIME             = 201;
static constexpr uint64_t SYS_GETEUID          = 107;
static constexpr uint64_t SYS_GETEGID          = 108;
static constexpr uint64_t SYS_GETPPID          = 110;
static constexpr uint64_t SYS_GETGROUPS        = 115;
static constexpr uint64_t SYS_SETGROUPS        = 116;
static constexpr uint64_t SYS_ARCH_PRCTL      = 158;
static constexpr uint64_t SYS_GETDENTS64      = 217;
static constexpr uint64_t SYS_SET_TID_ADDRESS = 218;
static constexpr uint64_t SYS_CLOCK_GETTIME   = 228;
static constexpr uint64_t SYS_CLOCK_NANOSLEEP = 230;
static constexpr uint64_t SYS_EXIT_GROUP      = 231;
static constexpr uint64_t SYS_OPENAT          = 257;
static constexpr uint64_t SYS_NEWFSTATAT      = 262;
static constexpr uint64_t SYS_READLINKAT      = 267;
static constexpr uint64_t SYS_SYMLINKAT       = 266;
static constexpr uint64_t SYS_PPOLL           = 271;
static constexpr uint64_t SYS_PIPE2           = 293;
static constexpr uint64_t SYS_EVENTFD2        = 290;
static constexpr uint64_t SYS_PRLIMIT64       = 302;
static constexpr uint64_t SYS_GETRANDOM       = 318;

// Additional syscalls
static constexpr uint64_t SYS_POLL            = 7;
static constexpr uint64_t SYS_RT_SIGRETURN    = 15;
static constexpr uint64_t SYS_SELECT          = 23;
static constexpr uint64_t SYS_SENDFILE        = 40;
static constexpr uint64_t SYS_KILL            = 62;
static constexpr uint64_t SYS_CHDIR           = 80;
static constexpr uint64_t SYS_FCHDIR          = 81;
static constexpr uint64_t SYS_RENAME          = 82;
static constexpr uint64_t SYS_MKDIR           = 83;
static constexpr uint64_t SYS_UNLINK          = 87;
static constexpr uint64_t SYS_SYMLINK         = 88;
static constexpr uint64_t SYS_READLINK        = 89;
static constexpr uint64_t SYS_UMASK           = 95;
static constexpr uint64_t SYS_GETRLIMIT       = 97;
static constexpr uint64_t SYS_GETRUSAGE       = 98;
static constexpr uint64_t SYS_SYSINFO         = 99;
static constexpr uint64_t SYS_SETPGID         = 109;
static constexpr uint64_t SYS_GETPGRP         = 111;
static constexpr uint64_t SYS_SETSID          = 112;
static constexpr uint64_t SYS_GETRESUID       = 118;
static constexpr uint64_t SYS_GETRESGID       = 120;
static constexpr uint64_t SYS_GETPGID         = 121;
static constexpr uint64_t SYS_GETSID          = 124;
static constexpr uint64_t SYS_SIGALTSTACK     = 131;
static constexpr uint64_t SYS_STATFS          = 137;
static constexpr uint64_t SYS_FSTATFS         = 138;
static constexpr uint64_t SYS_PRCTL           = 157;
static constexpr uint64_t SYS_GETTID          = 186;
static constexpr uint64_t SYS_FUTEX           = 202;
static constexpr uint64_t SYS_TKILL           = 200;
static constexpr uint64_t SYS_SET_ROBUST_LIST = 273;
static constexpr uint64_t SYS_TGKILL          = 234;
static constexpr uint64_t SYS_FACCESSAT       = 269;
static constexpr uint64_t SYS_DUP3            = 292;
static constexpr uint64_t SYS_RSEQ            = 334;
static constexpr uint64_t SYS_PSELECT6        = 270;
static constexpr uint64_t SYS_FACCESSAT2      = 439;
static constexpr uint64_t SYS_ALARM           = 37;
static constexpr uint64_t SYS_PAUSE           = 34;
static constexpr uint64_t SYS_RT_SIGSUSPEND   = 130;

// Nix package manager syscalls
static constexpr uint64_t SYS_FLOCK           = 73;
static constexpr uint64_t SYS_TRUNCATE        = 76;
static constexpr uint64_t SYS_FTRUNCATE       = 77;
static constexpr uint64_t SYS_LINK            = 86;
static constexpr uint64_t SYS_CHMOD           = 90;
static constexpr uint64_t SYS_FCHMOD          = 91;
static constexpr uint64_t SYS_CHOWN           = 92;
static constexpr uint64_t SYS_FCHOWN          = 93;
static constexpr uint64_t SYS_LCHOWN          = 94;
static constexpr uint64_t SYS_SCHED_SETAFFINITY = 203;
static constexpr uint64_t SYS_SCHED_GETAFFINITY = 204;
static constexpr uint64_t SYS_MKDIRAT         = 258;
static constexpr uint64_t SYS_FCHOWNAT        = 260;
static constexpr uint64_t SYS_UNLINKAT        = 263;
static constexpr uint64_t SYS_RENAMEAT        = 264;
static constexpr uint64_t SYS_LINKAT          = 265;
static constexpr uint64_t SYS_FCHMODAT        = 268;
static constexpr uint64_t SYS_UTIMENSAT       = 280;
static constexpr uint64_t SYS_FALLOCATE       = 285;
static constexpr uint64_t SYS_RENAMEAT2       = 316;
static constexpr uint64_t SYS_STATX           = 332;

// Socket syscalls
static constexpr uint64_t SYS_SOCKET          = 41;
static constexpr uint64_t SYS_CONNECT         = 42;
static constexpr uint64_t SYS_ACCEPT          = 43;
static constexpr uint64_t SYS_SENDTO          = 44;
static constexpr uint64_t SYS_RECVFROM        = 45;
static constexpr uint64_t SYS_SENDMSG         = 46;
static constexpr uint64_t SYS_RECVMSG         = 47;
static constexpr uint64_t SYS_SHUTDOWN        = 48;
static constexpr uint64_t SYS_BIND            = 49;
static constexpr uint64_t SYS_LISTEN          = 50;
static constexpr uint64_t SYS_GETSOCKNAME     = 51;
static constexpr uint64_t SYS_GETPEERNAME     = 52;
static constexpr uint64_t SYS_SOCKETPAIR      = 53;
static constexpr uint64_t SYS_SETSOCKOPT      = 54;
static constexpr uint64_t SYS_GETSOCKOPT      = 55;

// Wayland/epoll syscalls
static constexpr uint64_t SYS_EPOLL_CREATE    = 213;
static constexpr uint64_t SYS_EPOLL_WAIT      = 232;
static constexpr uint64_t SYS_EPOLL_CTL       = 233;
static constexpr uint64_t SYS_EPOLL_PWAIT     = 281;
static constexpr uint64_t SYS_TIMERFD_CREATE  = 283;
static constexpr uint64_t SYS_TIMERFD_SETTIME = 286;
static constexpr uint64_t SYS_TIMERFD_GETTIME = 287;
static constexpr uint64_t SYS_ACCEPT4         = 288;
static constexpr uint64_t SYS_EPOLL_CREATE1   = 291;
static constexpr uint64_t SYS_MEMFD_CREATE    = 319;
static constexpr uint64_t SYS_MEMFD_SECRET    = 447;

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

// Get the syscall table address as uint64_t (for per-CPU env setup).
uint64_t SyscallGetTableAddress();

// MemFd (memfd_create / shm_open) handle refcount helpers.
// The handle is a `MemFdData*` stored as `FdEntry::handle` on FdType::MemFd fds.
// process.cpp's fork path uses these to correctly inherit MemFd fds into the
// child without double-freeing the underlying buffer on close.
void MemFdHandleRef(void* handle);
void MemFdHandleUnref(void* handle);
void UnixSocketHandleRef(void* handle);

// Switch to user mode.
// Saves kernel state, builds IRETQ frame, SWAPGS, jumps to ring 3.
// When the user process calls sys_exit, control returns here.
void SwitchToUserMode(uint64_t userStack, uint64_t userEntry);

} // namespace brook
