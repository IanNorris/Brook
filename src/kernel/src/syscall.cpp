#include "brook_core_dump.h"
#include "gdt.h"
#include "chrome_shell_wire.h"
#include "shell_service.h"
#include "../../shared/brook_pointer.h"
// syscall.cpp -- SYSCALL/SYSRET dispatcher, syscall table, and user-mode entry.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "gpu_buffer_abi.h"
#include "gpu_window_source_abi.h"
#include "desktop_layers_export.h"
#include "system_message.h"
#include "fpu_irq.h"
#include "memory/virtual_memory.h"
#include "memory/pageable_memory.h"
#include "memory/mapping_fault_test.h"
#include "register_test.h"
#include "../../shared/brook_debug_frame.h"
#include "ahci_readonly.h"
#include "audio.h"
#include "syscall_timing.h"
#include "user_copy.h"
#include "gs_paranoid.h"
#include "syscall_internal.h"
#include "syscall_strace.h"
#include "syscall_dispatch.h"
#include "syscall_handlers.h"
#include "seccomp.h"
#include "compat_census.h"
#include "compat_census_runtime.h"

// Forward declaration
extern "C" __attribute__((naked)) void ReturnToKernel();
extern "C" bool MemFdHandleUserFault(uint64_t cr2, uint64_t errCode);
extern "C" bool FileMapHandleUserFault(uint64_t cr2, uint64_t errCode);

using brook::UserBufferReadable;
using brook::UserBufferWritable;
using brook::CopyToUser;
using brook::CopyUserCString;

// ---------------------------------------------------------------------------
// C dispatch wrapper — reads syscall number from GS:120, applies strace.
// Must be extern "C" for the assembly call instruction.
// ---------------------------------------------------------------------------

namespace brook { int64_t SyscallDispatchInternal(uint64_t num, uint64_t a0, uint64_t a1,
                                                   uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5); }

extern "C" int64_t SyscallDispatchC(uint64_t a0, uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t num;
    asm volatile("movq %%gs:120, %0" : "=r"(num));

    return brook::SyscallDispatchInternal(num, a0, a1, a2, a3, a4, a5);
}

extern "C" void SyscallSnapshotCurrentC()
{
    uint64_t procPtr;
    uint64_t userRip, userRsp, userRflags;
    uint64_t userRbx, userRbp, userR12, userR13, userR14, userR15;
    uint64_t userRdi, userRsi, userRdx, userR8, userR9, userR10;
    uint64_t syscallNum;

    asm volatile("movq %%gs:184, %0" : "=r"(procPtr));
    asm volatile("movq %%gs:48,  %0" : "=r"(userRip));
    asm volatile("movq %%gs:56,  %0" : "=r"(userRsp));
    asm volatile("movq %%gs:64,  %0" : "=r"(userRflags));
    asm volatile("movq %%gs:72,  %0" : "=r"(userRbx));
    asm volatile("movq %%gs:80,  %0" : "=r"(userRbp));
    asm volatile("movq %%gs:88,  %0" : "=r"(userR12));
    asm volatile("movq %%gs:96,  %0" : "=r"(userR13));
    asm volatile("movq %%gs:104, %0" : "=r"(userR14));
    asm volatile("movq %%gs:112, %0" : "=r"(userR15));
    asm volatile("movq %%gs:128, %0" : "=r"(userRdi));
    asm volatile("movq %%gs:136, %0" : "=r"(userRsi));
    asm volatile("movq %%gs:144, %0" : "=r"(userRdx));
    asm volatile("movq %%gs:152, %0" : "=r"(userR8));
    asm volatile("movq %%gs:160, %0" : "=r"(userR9));
    asm volatile("movq %%gs:168, %0" : "=r"(userR10));
    asm volatile("movq %%gs:120, %0" : "=r"(syscallNum));

    auto* proc = reinterpret_cast<brook::Process*>(procPtr);
    if (!proc)
        return;

    proc->syscallUserRip = userRip;
    proc->syscallUserRsp = userRsp;
    proc->syscallUserRflags = userRflags;
    proc->syscallUserRbx = userRbx;
    proc->syscallUserRbp = userRbp;
    proc->syscallUserR12 = userR12;
    proc->syscallUserR13 = userR13;
    proc->syscallUserR14 = userR14;
    proc->syscallUserR15 = userR15;
    proc->syscallUserRdi = userRdi;
    proc->syscallUserRsi = userRsi;
    proc->syscallUserRdx = userRdx;
    proc->syscallUserR8 = userR8;
    proc->syscallUserR9 = userR9;
    proc->syscallUserR10 = userR10;
    proc->currentSyscallNum = syscallNum;
    brook::CoreDumpPublishRegisters(proc,{
        userR15,userR14,userR13,userR12,userRbp,userRbx,userRflags,userR10,userR9,userR8,
        syscallNum,userRip,userRdx,userRsi,userRdi,syscallNum,userRip,GDT_USER_CODE,userRflags,
        userRsp,GDT_USER_DATA,proc->fsBase,proc->savedCtx.gsBase,0,0,0,0
    });
}

// ---------------------------------------------------------------------------
// SYSCALL dispatcher -- naked assembly, pointed to by LSTAR MSR.
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked, used)) void BrookSyscallDispatcher()
{
    __asm__ volatile(
        "swapgs\n\t"
        "movq %%r11, %%gs:24\n\t"      // stash user RFLAGS
        "mov %%rsp, %%r11\n\t"          // r11 = user RSP
        "mov %%gs:8, %%rsp\n\t"
        "and $~0xF, %%rsp\n\t"
        // Keep all existing frame/RBP offsets and 16-byte call alignment.
        "sub $64, %%rsp\n\t"
        "movq $0, (%%rsp)\n\t"          // this entry defaults to SYSRET
        "push %%r11\n\t"               // [1] user RSP
        "push %%rcx\n\t"               // [2] user return address
        "movq %%gs:24, %%r11\n\t"       // reload user RFLAGS
        "push %%r11\n\t"               // [3] user RFLAGS
        "push %%rbp\n\t"               // [4] user RBP
        "mov %%rsp, %%rbp\n\t"
        "push %%rdx\n\t"               // [5]
        "push %%rsi\n\t"               // [6]
        "push %%rdi\n\t"               // [7]
        "push %%r8\n\t"                // [8]
        "push %%r9\n\t"                // [9]
        "push %%r10\n\t"               // [10]
        "push %%r11\n\t"               // [11]
        "push %%r12\n\t"               // [12]
        "push %%r13\n\t"               // [13]
        "push %%r14\n\t"               // [14]
        "push %%r15\n\t"               // [15]
        "push %%rbx\n\t"               // [16] — 16 pushes = 128 bytes, aligned
        "mov %%r10, %%rcx\n\t"
        "movq %%rax, %%gs:120\n\t"       // save syscall number for debug
        "mov %%gs:16, %%r12\n\t"
        // Save user context in KernelCpuEnv for fork().
        // Use R15 as temp (saved at [rbp-88]).
        "mov 16(%%rbp), %%r15\n\t"       // user RCX (return addr)
        "movq %%r15, %%gs:48\n\t"        // -> syscallUserRip
        "mov 24(%%rbp), %%r15\n\t"       // user RSP
        "movq %%r15, %%gs:56\n\t"        // -> syscallUserRsp
        "mov 8(%%rbp), %%r15\n\t"        // user RFLAGS (R11)
        "movq %%r15, %%gs:64\n\t"        // -> syscallUserRflags
        // Save callee-saved registers for fork child
        "mov -96(%%rbp), %%r15\n\t"      // RBX at [rbp-96]
        "movq %%r15, %%gs:72\n\t"        // -> syscallUserRbx
        "mov 0(%%rbp), %%r15\n\t"        // user RBP at [rbp+0]
        "movq %%r15, %%gs:80\n\t"        // -> syscallUserRbp
        "mov -64(%%rbp), %%r15\n\t"      // R12 at [rbp-64]
        "movq %%r15, %%gs:88\n\t"        // -> syscallUserR12
        "mov -72(%%rbp), %%r15\n\t"      // R13 at [rbp-72]
        "movq %%r15, %%gs:96\n\t"        // -> syscallUserR13
        "mov -80(%%rbp), %%r15\n\t"      // R14 at [rbp-80]
        "movq %%r15, %%gs:104\n\t"       // -> syscallUserR14
        "mov -88(%%rbp), %%r15\n\t"      // R15 at [rbp-88]
        "movq %%r15, %%gs:112\n\t"       // -> syscallUserR15
        // Caller-saved registers (needed for fork — Linux preserves all regs)
        "mov -24(%%rbp), %%r15\n\t"      // RDI at [rbp-24]
        "movq %%r15, %%gs:128\n\t"       // -> syscallUserRdi
        "mov -16(%%rbp), %%r15\n\t"      // RSI at [rbp-16]
        "movq %%r15, %%gs:136\n\t"       // -> syscallUserRsi
        "mov -8(%%rbp), %%r15\n\t"       // RDX at [rbp-8]
        "movq %%r15, %%gs:144\n\t"       // -> syscallUserRdx
        "mov -32(%%rbp), %%r15\n\t"      // R8 at [rbp-32]
        "movq %%r15, %%gs:152\n\t"       // -> syscallUserR8
        "mov -40(%%rbp), %%r15\n\t"      // R9 at [rbp-40]
        "movq %%r15, %%gs:160\n\t"       // -> syscallUserR9
        "mov -48(%%rbp), %%r15\n\t"      // R10 at [rbp-48]
        "movq %%r15, %%gs:168\n\t"       // -> syscallUserR10
        // Syscalls preserve user vector state even though the C handlers may
        // use SSE. Reuse the IRQ stack-save convention before the first C
        // call: R14 retains the unchanged GPR-frame base, R15 the 64-byte
        // aligned XSAVE area. Both are callee-saved across blocking switches.
        // At most 1215 additional bytes are reserved on the 256 KiB stack;
        // the save area sits above the C stack and below the GPR frame.
        BROOK_FPU_SAVE_IRQ
        "call SyscallSnapshotCurrentC\n\t"
        "mov -24(%%rbp), %%rdi\n\t"      // a0
        "mov -16(%%rbp), %%rsi\n\t"      // a1
        "mov -8(%%rbp), %%rdx\n\t"       // a2
        "mov -48(%%rbp), %%rcx\n\t"      // a3 (user R10)
        "mov -32(%%rbp), %%r8\n\t"       // a4
        "mov -40(%%rbp), %%r9\n\t"       // a5
        "sti\n\t"
        "call SyscallDispatchC\n\t"
        "cli\n\t"
        // Check for pending signals before returning to userspace.
        // Pass the saved-register frame and syscall return value.
        "mov %%r14, %%rdi\n\t"         // original SyscallFrame, above XSAVE area
        "mov %%rax, %%rsi\n\t"         // RSI = syscall result
        "mov %%r15, %%rdx\n\t"        // authoritative entry XSAVE slot
        "call SyscallCheckSignals\n\t"  // returns new RAX
        "jmp .Lsyscall_return\n\t"
        ".Lsyscall_invalid:\n\t"
        "mov $-38, %%rax\n\t"
        ".Lsyscall_return:\n\t"
        // XRSTOR uses EDX:EAX as its mask. Preserve the final syscall result
        // (including rt_sigreturn's restored RAX) in a saved scratch register.
        "mov %%rax, %%r13\n\t"
        BROOK_FPU_RESTORE_IRQ
        "mov %%r13, %%rax\n\t"
        "pop %%rbx\n\t"                // [16]
        "pop %%r15\n\t"                // [15]
        "pop %%r14\n\t"                // [14]
        "pop %%r13\n\t"                // [13]
        "pop %%r12\n\t"                // [12]
        "pop %%r11\n\t"                // [11]
        "pop %%r10\n\t"                // [10]
        "pop %%r9\n\t"                 // [9]
        "pop %%r8\n\t"                 // [8]
        "pop %%rdi\n\t"                // [7]
        "pop %%rsi\n\t"                // [6]
        "pop %%rdx\n\t"                // [5]
        "pop %%rbp\n\t"                // [4]
        // RSP is now frame+104. Signal return must restore RCX and R11 as
        // ordinary GPRs; SYSRET would replace them with RIP and RFLAGS.
        "cmpq $0, 24(%%rsp)\n\t"        // returnState.mode at frame+128
        "jne .Lsigreturn_iret\n\t"
        "pop %%r11\n\t"                // [3] user RFLAGS
        "pop %%rcx\n\t"                // [2] user return address
        // Every high bit must be clear for Brook's low-47-bit user range.
        // Bit 47 alone misses noncanonical values with higher bits set.
        // The scratch push reuses the consumed RIP slot, preserving RAX/RSP.
        "push %%rax\n\t"
        "mov %%rcx, %%rax\n\t"
        "shr $47, %%rax\n\t"
        "test %%rax, %%rax\n\t"
        "pop %%rax\n\t"
        "jnz .Lsysret_bad_rcx\n\t"
        "swapgs\n\t"
        "mov (%%rsp), %%rsp\n\t"       // [1] user RSP
        ".byte 0x48\n\t"
        "sysret\n\t"
        ".Lsysret_bad_rcx:\n\t"
        "mov $0x3F8, %%dx\n\t"
        "mov $0x58, %%al\n\t"
        "outb %%al, (%%dx)\n\t"
        "int3\n\t"
        "cli\n\t"
        ".Lsysret_halt:\n\t"
        "hlt\n\t"
        "jmp .Lsysret_halt\n\t"
        ".Lsigreturn_iret:\n\t"
        "mov 32(%%rsp), %%rcx\n\t"      // frame+136, original signal RCX
        "add $40, %%rsp\n\t"            // frame+144, validated IRET frame
        IRETQ_NORMALIZE_RFLAGS
        "swapgs\n\t"
        "iretq\n\t"
        ::: "memory"
    );
}
namespace brook {

// ---------------------------------------------------------------------------
// sys_not_implemented
// ---------------------------------------------------------------------------

static int64_t sys_not_implemented(uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    // Rate-limit: log first 8 per-syscall, then every 256th, to avoid
    // serial lock contention when many processes hit unimplemented syscalls.
    static volatile uint32_t s_unimplCount = 0;
    uint32_t n = __atomic_fetch_add(&s_unimplCount, 1, __ATOMIC_RELAXED);

    Process* proc = ProcessCurrent();
    uint64_t syscallNum = 0;
    __asm__ volatile("mov %%gs:120, %0" : "=r"(syscallNum));

    if (n < 8 || (n & 0xFF) == 0)
        SerialPrintf("UNIMPL: syscall %lu from pid %u ('%s') [#%u]\n",
                     syscallNum, proc ? proc->pid : 0, proc ? proc->name : "?", n);
    // Every hit is also counted per syscall and process in the compat
    // census (compat_census.cpp), which the dispatcher feeds.
    return -ENOSYS;
}

// Silent ENOSYS: for syscalls we deliberately don't implement but where
// the userland-side fallback is well-trodden (e.g. inotify -> polling).
// Avoids noisy UNIMPL spam during normal operation.
static int64_t sys_enosys_quiet(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS;
}

// Lives in its own page (.syscall_table) so we can mark it read-only
// after init. See VmmKernelMarkReadOnly + linker.ld.
SyscallFn g_syscallTable[SYSCALL_MAX]
    __attribute__((section(".syscall_table"), aligned(4096)));

extern "C" uint8_t __syscall_table_start[];
extern "C" uint8_t __syscall_table_end[];

void SyscallTableInit()
{
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        g_syscallTable[i] = sys_not_implemented;

    g_syscallTable[SYS_READ]            = sys_read;
    g_syscallTable[SYS_WRITE]           = sys_write;
    g_syscallTable[SYS_OPEN]            = sys_open;
    g_syscallTable[SYS_CREAT]           = sys_creat;
    g_syscallTable[SYS_CLOSE]           = sys_close;
    g_syscallTable[SYS_STAT]            = sys_stat;
    g_syscallTable[SYS_FSTAT]           = sys_fstat;
    g_syscallTable[SYS_LSTAT]           = sys_lstat;
    g_syscallTable[SYS_LSEEK]           = sys_lseek;
    g_syscallTable[SYS_MMAP]            = sys_mmap;
    g_syscallTable[SYS_MPROTECT]        = sys_mprotect;
    g_syscallTable[SYS_MUNMAP]          = sys_munmap;
    g_syscallTable[SYS_MREMAP]          = sys_mremap;
    g_syscallTable[SYS_MSYNC]           = sys_msync;
    g_syscallTable[SYS_BRK]             = sys_brk;
    g_syscallTable[SYS_RT_SIGACTION]    = sys_rt_sigaction;
    g_syscallTable[SYS_RT_SIGPROCMASK]  = sys_rt_sigprocmask;
    g_syscallTable[SYS_IOCTL]           = sys_ioctl;
    g_syscallTable[SYS_READV]           = sys_readv;
    g_syscallTable[SYS_WRITEV]          = sys_writev;
    g_syscallTable[SYS_ACCESS]          = sys_access;
    g_syscallTable[SYS_SCHED_YIELD]     = sys_sched_yield;
    g_syscallTable[324]                = sys_membarrier;
    g_syscallTable[SYS_NANOSLEEP]       = sys_nanosleep;
    g_syscallTable[SYS_GETPID]          = sys_getpid;
    g_syscallTable[SYS_PIPE]            = sys_pipe;
    g_syscallTable[SYS_DUP]             = sys_dup;
    g_syscallTable[SYS_DUP2]            = sys_dup2;
    g_syscallTable[SYS_CLONE]           = sys_clone;
    g_syscallTable[SYS_FORK]            = sys_fork;
    g_syscallTable[SYS_VFORK]           = sys_vfork;
    g_syscallTable[435]                 = sys_clone3;
    g_syscallTable[SYS_EXECVE]          = sys_execve;
    g_syscallTable[SYS_EXIT]            = sys_exit;
    g_syscallTable[SYS_WAIT4]           = sys_wait4;
    g_syscallTable[SYS_WAITID]          = sys_waitid;
    g_syscallTable[SYS_UNAME]           = sys_uname;
    g_syscallTable[170]                 = sys_sethostname;
    g_syscallTable[171]                 = sys_setdomainname;
    g_syscallTable[272]                 = sys_unshare;
    g_syscallTable[SYS_FCNTL]           = sys_fcntl;
    g_syscallTable[SYS_FSYNC]           = sys_fsync;
    g_syscallTable[SYS_FDATASYNC]       = sys_fsync;
    g_syscallTable[SYS_SYNC]            = sys_sync;
    g_syscallTable[SYS_PIVOT_ROOT]      = sys_pivot_root;
    g_syscallTable[SYS_MOUNT]           = sys_mount;
    g_syscallTable[SYS_UMOUNT2]         = sys_umount2;
    g_syscallTable[SYS_GETCWD]          = sys_getcwd;
    g_syscallTable[SYS_GETTIMEOFDAY]    = sys_gettimeofday;
    g_syscallTable[SYS_TIME]             = sys_time;
    g_syscallTable[SYS_GETUID]          = sys_getuid;
    g_syscallTable[SYS_GETGID]          = sys_getgid;
    g_syscallTable[SYS_SETUID]          = sys_setuid;
    g_syscallTable[SYS_SETGID]          = sys_setgid;
    g_syscallTable[113] = sys_setreuid;
    g_syscallTable[114] = sys_setregid;
    g_syscallTable[117] = sys_setresuid;
    g_syscallTable[119] = sys_setresgid;
    g_syscallTable[SYS_CAPGET]          = sys_capget;
    g_syscallTable[SYS_CAPSET]          = sys_capset;
    // inotify: deliberately unimplemented. Glibc/GLib fall back to
    // polling/manual checks when inotify_init fails. Returning ENOSYS
    // quietly avoids UNIMPL serial spam from GIMP / file pickers.
    g_syscallTable[253]                  = sys_enosys_quiet; // inotify_init
    g_syscallTable[254]                  = sys_enosys_quiet; // inotify_add_watch
    g_syscallTable[255]                  = sys_enosys_quiet; // inotify_rm_watch
    g_syscallTable[294]                  = sys_enosys_quiet; // inotify_init1
    g_syscallTable[SYS_GETEUID]         = sys_geteuid;
    g_syscallTable[SYS_GETEGID]         = sys_getegid;
    g_syscallTable[SYS_GETPPID]         = sys_getppid;
    g_syscallTable[SYS_GETGROUPS]       = sys_getgroups;
    g_syscallTable[SYS_SETGROUPS]       = sys_setgroups;
    g_syscallTable[SYS_ARCH_PRCTL]      = sys_arch_prctl;
    g_syscallTable[SYS_GETDENTS64]      = sys_getdents64;
    g_syscallTable[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    g_syscallTable[SYS_CLOCK_GETTIME]   = sys_clock_gettime;
    g_syscallTable[SYS_CLOCK_GETRES]    = sys_clock_getres;
    g_syscallTable[SYS_CLOCK_NANOSLEEP] = sys_clock_nanosleep;
    g_syscallTable[SYS_EXIT_GROUP]      = sys_exit_group;
    g_syscallTable[SYS_OPENAT]          = sys_openat;
    g_syscallTable[SYS_NEWFSTATAT]      = sys_newfstatat;
    g_syscallTable[SYS_PRLIMIT64]       = sys_prlimit64;
    g_syscallTable[SYS_GETRANDOM]       = sys_getrandom;

    // New syscalls
    g_syscallTable[SYS_POLL]            = sys_poll;
    g_syscallTable[SYS_RT_SIGRETURN]    = sys_rt_sigreturn;
    g_syscallTable[SYS_SELECT]          = sys_select;
    g_syscallTable[SYS_PSELECT6]        = sys_pselect6;
    g_syscallTable[SYS_SENDFILE]        = sys_sendfile;
    g_syscallTable[SYS_KILL]            = sys_kill;
    g_syscallTable[SYS_CHDIR]           = sys_chdir;
    g_syscallTable[SYS_FCHDIR]          = sys_fchdir;
    g_syscallTable[SYS_RENAME]          = sys_rename;
    g_syscallTable[SYS_MKNOD] = sys_mknod;
    g_syscallTable[SYS_MKNODAT] = sys_mknodat;
    g_syscallTable[SYS_MKDIR]           = sys_mkdir;
    g_syscallTable[SYS_RMDIR]           = sys_rmdir;
    g_syscallTable[SYS_UNLINK]          = sys_unlink;
    g_syscallTable[SYS_SYMLINK]         = sys_symlink;
    g_syscallTable[SYS_READLINK]        = sys_readlink;
    g_syscallTable[SYS_UMASK]           = sys_umask;
    g_syscallTable[SYS_GETRLIMIT]       = sys_getrlimit;
    g_syscallTable[SYS_SETRLIMIT]       = sys_setrlimit;
    g_syscallTable[SYS_GETRUSAGE]       = sys_getrusage;
    g_syscallTable[SYS_SYSINFO]         = sys_sysinfo;
    g_syscallTable[SYS_TIMES]           = sys_times;
    g_syscallTable[SYS_SETPGID]         = sys_setpgid;
    g_syscallTable[SYS_GETPGRP]         = sys_getpgrp;
    g_syscallTable[SYS_SETSID]          = sys_setsid;
    g_syscallTable[SYS_GETPGID]         = sys_getpgid;
    g_syscallTable[SYS_GETSID]          = sys_getsid;
    g_syscallTable[SYS_SIGALTSTACK]     = sys_sigaltstack;
    g_syscallTable[SYS_ALARM]           = sys_alarm;
    g_syscallTable[SYS_GETITIMER]       = sys_getitimer;
    g_syscallTable[312]                 = sys_kcmp;
    g_syscallTable[SYS_SETITIMER]       = sys_setitimer;
    g_syscallTable[SYS_PAUSE]           = sys_pause;
    g_syscallTable[SYS_RT_SIGPENDING] = sys_rt_sigpending;
    g_syscallTable[SYS_RT_SIGTIMEDWAIT] = sys_rt_sigtimedwait;
    g_syscallTable[SYS_RT_SIGSUSPEND]   = sys_rt_sigsuspend;
    g_syscallTable[SYS_STATFS]          = sys_statfs;
    g_syscallTable[SYS_FSTATFS]         = sys_fstatfs;
    g_syscallTable[SYS_GETTID]          = sys_gettid;
    g_syscallTable[SYS_TKILL]           = sys_tkill;
    g_syscallTable[SYS_TGKILL]          = sys_tgkill;
    g_syscallTable[SYS_READLINKAT]      = sys_readlinkat;
    g_syscallTable[SYS_SYMLINKAT]       = sys_symlinkat;
    g_syscallTable[SYS_PPOLL]           = sys_ppoll;
    g_syscallTable[SYS_PIPE2]           = sys_pipe2;
    g_syscallTable[SYS_EVENTFD2]        = sys_eventfd2;
    g_syscallTable[SYS_DUP3]            = sys_dup3;

    // Bash / POSIX compatibility
    g_syscallTable[SYS_PREAD64]         = sys_pread64;
    g_syscallTable[SYS_GETRESUID]       = sys_getresuid;
    g_syscallTable[SYS_GETRESGID]       = sys_getresgid;
    g_syscallTable[SYS_PRCTL]           = sys_prctl;
    g_syscallTable[317]                  = SysSeccomp; // Linux x86-64 seccomp
    g_syscallTable[SYS_FUTEX]           = sys_futex;
    g_syscallTable[SYS_SIGNALFD]        = sys_signalfd;
    g_syscallTable[SYS_SIGNALFD4]       = sys_signalfd4;
    g_syscallTable[240] = sys_mq_open;
    g_syscallTable[241] = sys_mq_unlink;
    g_syscallTable[242] = sys_mq_timedsend;
    g_syscallTable[243] = sys_mq_timedreceive;
    g_syscallTable[245] = sys_mq_getsetattr;
    g_syscallTable[SYS_SEMGET]          = sys_semget;
    g_syscallTable[SYS_SEMOP]           = sys_semop;
    g_syscallTable[SYS_SEMTIMEDOP]      = sys_semtimedop;
    g_syscallTable[SYS_SEMCTL]          = sys_semctl;
    g_syscallTable[SYS_SET_ROBUST_LIST] = sys_set_robust_list;
    g_syscallTable[SYS_GET_ROBUST_LIST] = sys_get_robust_list;
    g_syscallTable[SYS_FACCESSAT]       = sys_faccessat;
    // Deliberate ENOSYS with established userspace fallbacks: counted by the
    // compat census in its quiet set, never logged as UNIMPL.
    g_syscallTable[SYS_RSEQ]            = sys_enosys_quiet; // glibc/musl run without rseq
    g_syscallTable[329]                 = sys_enosys_quiet; // pkey_mprotect
    g_syscallTable[330]                 = sys_enosys_quiet; // pkey_alloc: no PKU; V8/PartitionAlloc fall back
    g_syscallTable[331]                 = sys_enosys_quiet; // pkey_free
    g_syscallTable[425]                 = sys_enosys_quiet; // io_uring_setup: libuv/glib use epoll
    g_syscallTable[426]                 = sys_enosys_quiet; // io_uring_enter
    g_syscallTable[427]                 = sys_enosys_quiet; // io_uring_register
    g_syscallTable[444]                 = sys_enosys_quiet; // landlock_create_ruleset: sandboxes skip it
    g_syscallTable[445]                 = sys_enosys_quiet; // landlock_add_rule
    g_syscallTable[446]                 = sys_enosys_quiet; // landlock_restrict_self
    g_syscallTable[SYS_CLOSE_RANGE]     = sys_close_range;
    g_syscallTable[SYS_FACCESSAT2]      = sys_faccessat2;

    // Nix package manager syscalls
    g_syscallTable[SYS_FLOCK]           = sys_flock;
    g_syscallTable[SYS_TRUNCATE]        = sys_truncate;
    g_syscallTable[SYS_FTRUNCATE]       = sys_ftruncate;
    g_syscallTable[SYS_LINK]            = sys_link;
    g_syscallTable[SYS_CHMOD]           = sys_chmod;
    g_syscallTable[SYS_FCHMOD]          = sys_fchmod;
    g_syscallTable[SYS_CHOWN]           = sys_chown;
    g_syscallTable[SYS_FCHOWN]          = sys_fchown;
    g_syscallTable[SYS_LCHOWN]          = sys_lchown;
    g_syscallTable[SYS_SCHED_SETPARAM]  = sys_sched_setparam;
    g_syscallTable[SYS_SCHED_GETPARAM]  = sys_sched_getparam;
    g_syscallTable[SYS_SCHED_SETAFFINITY] = sys_sched_setaffinity;
    g_syscallTable[SYS_SCHED_GETAFFINITY] = sys_sched_getaffinity;
    g_syscallTable[SYS_SCHED_SETSCHEDULER] = sys_sched_setscheduler;
    g_syscallTable[SYS_SCHED_GETSCHEDULER] = sys_sched_getscheduler;
    g_syscallTable[SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max;
    g_syscallTable[SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min;
    g_syscallTable[SYS_MKDIRAT]         = sys_mkdirat;
    g_syscallTable[SYS_FCHOWNAT]        = sys_fchownat;
    g_syscallTable[SYS_UNLINKAT]        = sys_unlinkat;
    g_syscallTable[SYS_RENAMEAT]        = sys_renameat;
    g_syscallTable[SYS_LINKAT]          = sys_linkat;
    g_syscallTable[SYS_FCHMODAT]        = sys_fchmodat;
    g_syscallTable[SYS_UTIMENSAT]       = sys_utimensat;
    g_syscallTable[SYS_FALLOCATE]       = sys_fallocate;
    g_syscallTable[SYS_RENAMEAT2]       = sys_renameat2;
    g_syscallTable[SYS_STATX]           = sys_statx;
    g_syscallTable[SYS_MINCORE]         = sys_mincore;
    g_syscallTable[SYS_MADVISE]         = sys_madvise;
    g_syscallTable[SYS_SHMGET]          = sys_enosys_quiet;
    g_syscallTable[SYS_GETPRIORITY]     = sys_getpriority;
    g_syscallTable[SYS_SETPRIORITY]     = sys_setpriority;
    g_syscallTable[SYS_PIDFD_OPEN]      = sys_enosys_quiet;
    g_syscallTable[SYS_FADVISE64]       = sys_fadvise64;
    g_syscallTable[SYS_MBIND]           = sys_mbind;
    g_syscallTable[SYS_SET_MEMPOLICY]   = sys_enosys_quiet;
    g_syscallTable[SYS_GET_MEMPOLICY]   = sys_enosys_quiet;
    g_syscallTable[SYS_PWRITE64]        = sys_pwrite64;

    // Socket syscalls
    g_syscallTable[SYS_SOCKET]          = sys_socket;
    g_syscallTable[SYS_CONNECT]         = sys_connect;
    g_syscallTable[SYS_ACCEPT]          = sys_accept;
    g_syscallTable[SYS_ACCEPT4]         = sys_accept4;
    g_syscallTable[SYS_SENDTO]          = sys_sendto;
    g_syscallTable[SYS_RECVFROM]        = sys_recvfrom;
    g_syscallTable[SYS_SENDMSG]         = sys_sendmsg;
    g_syscallTable[SYS_RECVMSG]         = sys_recvmsg;
    g_syscallTable[SYS_SENDMMSG]        = sys_sendmmsg;
    g_syscallTable[SYS_RECVMMSG]        = sys_recvmmsg;
    g_syscallTable[SYS_SHUTDOWN]        = sys_shutdown;
    g_syscallTable[SYS_BIND]            = sys_bind;
    g_syscallTable[SYS_LISTEN]          = sys_listen;
    g_syscallTable[SYS_GETSOCKNAME]     = sys_getsockname;
    g_syscallTable[SYS_GETPEERNAME]     = sys_getpeername;
    g_syscallTable[SYS_SOCKETPAIR]      = sys_socketpair;
    g_syscallTable[SYS_SETSOCKOPT]      = sys_setsockopt;
    g_syscallTable[SYS_GETSOCKOPT]      = sys_getsockopt;

    // Wayland prerequisites
    g_syscallTable[SYS_EPOLL_CREATE]    = sys_epoll_create;
    g_syscallTable[SYS_EPOLL_CREATE1]   = sys_epoll_create1;
    g_syscallTable[SYS_EPOLL_CTL]       = sys_epoll_ctl;
    g_syscallTable[SYS_EPOLL_WAIT]      = sys_epoll_wait;
    g_syscallTable[SYS_EPOLL_PWAIT]     = sys_epoll_pwait;
    g_syscallTable[SYS_TIMER_CREATE] = sys_timer_create;
    g_syscallTable[SYS_TIMER_SETTIME] = sys_timer_settime;
    g_syscallTable[SYS_TIMER_GETTIME] = sys_timer_gettime;
    g_syscallTable[SYS_TIMER_GETOVERRUN] = sys_timer_getoverrun;
    g_syscallTable[SYS_TIMER_DELETE] = sys_timer_delete;
    g_syscallTable[SYS_TIMERFD_CREATE]  = sys_timerfd_create;
    g_syscallTable[SYS_TIMERFD_SETTIME] = sys_timerfd_settime;
    g_syscallTable[SYS_TIMERFD_GETTIME] = sys_timerfd_gettime;
    g_syscallTable[SYS_MEMFD_CREATE]    = sys_memfd_create;

    // Brook-specific syscalls (500+). 500 = profiler control.
    g_syscallTable[500]                  = sys_brook_profile;
    g_syscallTable[502]                  = sys_brook_set_crash_entry;
    g_syscallTable[503]                  = sys_brook_crash_complete;
    g_syscallTable[504]                  = sys_brook_input_pop;
    g_syscallTable[505]                  = sys_brook_input_grab;
    g_syscallTable[506]                  = sys_brook_wm_create_window;
    g_syscallTable[507]                  = sys_brook_wm_destroy_window;
    g_syscallTable[508]                  = sys_brook_wm_signal_dirty;
    g_syscallTable[509]                  = sys_brook_wm_set_title;
    g_syscallTable[510]                  = sys_brook_wm_pop_input;
    g_syscallTable[511]                  = sys_brook_wm_resize_vfb;
    g_syscallTable[512]                  = sys_brook_wm_set_decoration_mode;
    g_syscallTable[513]                  = sys_brook_wm_begin_move;
    g_syscallTable[514]                  = sys_brook_wm_move_relative;
    g_syscallTable[515]                  = sys_brook_wm_set_maximized;
    g_syscallTable[526]                  = sys_brook_wm_set_fullscreen;
    g_syscallTable[516]                  = sys_brook_wm_set_minimized;
    g_syscallTable[517]                  = sys_brook_wm_begin_resize;
    g_syscallTable[518]                  = sys_brook_wm_set_cursor_visible;
    g_syscallTable[519]                  = sys_brook_wm_set_cursor_image;
    g_syscallTable[520]                  = sys_brook_wm_get_screen_info;
    g_syscallTable[521]                  = sys_brook_wm_scanout_map;
    g_syscallTable[522]                  = sys_brook_wm_scanout_flip;
    g_syscallTable[523]                  = sys_brook_wm_get_windows;
    g_syscallTable[524]                  = sys_brook_wm_map_window_vfb;
    g_syscallTable[525]                  = sys_brook_wm_present_gres;
    // Brook windowing extensions live in a high 0xB00 block, clear of the Linux
    // and Windows syscall ranges.
    g_syscallTable[0xB00]                = sys_brook_wm_set_window_properties; // WM_SET_WINDOW_PROPERTIES
    g_syscallTable[0xB01]                = sys_brook_wm_set_app_id;
    g_syscallTable[0xB05]                = sys_brook_wm_set_client_socket;
    g_syscallTable[0xB06]                = sys_brook_scene_create;
    g_syscallTable[0xB07]                = sys_brook_scene_ctl;
    g_syscallTable[0xB08]                = sys_brook_wm_window_state;
    g_syscallTable[0xB09]                = sys_brook_wm_place_window;
    g_syscallTable[0xB0A]                = sys_brook_wm_pointer_capture;
    g_syscallTable[0xB25]                = sys_brook_wm_show_window_menu;
    g_syscallTable[BROOK_SYS_GPU_OUTPUT_ALLOC] = sys_brook_gpu_output_alloc;
    g_syscallTable[BROOK_SYS_GPU_BUFFER_TRY_CPU_READ] = sys_brook_gpu_buffer_try_cpu_read;
    g_syscallTable[BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE] = sys_brook_gpu_buffer_snapshot_fence;
    g_syscallTable[BROOK_SYS_GPU_WINDOW_VALIDATE] = sys_brook_gpu_window_validate;
    g_syscallTable[BROOK_SYS_GPU_WINDOW_PEER_RECOVERY] = sys_brook_gpu_window_peer_recovery;
    g_syscallTable[BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT] = sys_brook_wm_publish_source_commit;
    g_syscallTable[BROOK_SYS_WM_QUERY_SOURCE_COMMIT] = sys_brook_wm_query_source_commit;
    g_syscallTable[BROOK_SYS_WM_BEGIN_SOURCE_WRITE] = sys_brook_wm_begin_source_write;
    g_syscallTable[BROOK_SYS_GPU_HANDOFF_CTL] = sys_brook_gpu_handoff_ctl;
    g_syscallTable[BROOK_SYS_DESKTOP_LAYERS] = sys_brook_desktop_layers;
    g_syscallTable[0xB02]                = sys_brook_wm_theme;
    g_syscallTable[0xB03]                = sys_brook_wm_refresh_apps;
    g_syscallTable[0xB04]                = sys_brook_wm_scale;
    g_syscallTable[BROOK_SYS_DEBUG_FRAME] = sys_brook_debug_frame;
    g_syscallTable[BROOK_SYS_DESKTOP_OVERLAY] = sys_brook_desktop_overlay;
    g_syscallTable[brook::ShellServiceSyscall] = brook::ShellServiceSyscallInvoke;
    g_syscallTable[BROOK_SYS_AHCI_EXPORT] = sys_brook_ahci_export;
    g_syscallTable[BROOK_SYS_SYSTEM_MESSAGE] = sys_brook_system_message;
    g_syscallTable[BROOK_SYS_CORE_DUMP] = sys_brook_core_dump;
    g_syscallTable[0xB40]                = sys_brook_pageable_memory;
    g_syscallTable[BROOK_SYS_WM_APPEARANCE] = sys_brook_wm_appearance;
    g_syscallTable[BROOK_SYS_WM_TASKBAR_METRICS] = sys_brook_wm_taskbar_metrics;
    g_syscallTable[BROOK_SYS_POINTER_SPEED] = sys_brook_pointer_speed;
    g_syscallTable[BROOK_SYS_TOUCHPAD_TAP] = sys_brook_touchpad_tap;
    g_syscallTable[BROOK_SYS_AUDIO_CONTROL] = sys_brook_audio_control;
#ifdef BROOK_MAPPING_TEST_HOOKS
    g_syscallTable[0xB30]                = MappingFaultTestControl;
#endif
#ifdef BROOK_REGISTER_TEST_HOOKS
    g_syscallTable[0xB31]                = RegisterTestControl;
#endif
    // App-GPU (per-process virgl) syscalls for userspace GL rendering.
    g_syscallTable[0xB10]                = sys_brook_gpu_ctx_create;       // GPU_CTX_CREATE
    g_syscallTable[0xB11]                = sys_brook_gpu_resource_create;  // GPU_RESOURCE_CREATE
    g_syscallTable[0xB12]                = sys_brook_gpu_attach_window;    // GPU_ATTACH_WINDOW
    g_syscallTable[0xB16]                = sys_brook_gpu_upload_buffer;    // GPU_UPLOAD_BUFFER
    g_syscallTable[0xB13]                = sys_brook_gpu_submit;           // GPU_SUBMIT
    g_syscallTable[0xB14]                = sys_brook_gpu_transfer;         // GPU_TRANSFER
    g_syscallTable[0xB15]                = sys_brook_gpu_ctx_destroy;      // GPU_CTX_DESTROY

    uint32_t count = 0;
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        if (g_syscallTable[i] != sys_not_implemented) ++count;

    SerialPrintf("SYSCALL: table initialised (%u entries, %u implemented)\n",
                 static_cast<unsigned>(SYSCALL_MAX), count);

    // Lock the dispatch table read-only. Any subsequent write attempt
    // (corruption, stray pointer) will #PF immediately on the offending
    // RIP rather than silently leaving a null entry to be discovered
    // later by an indirect call. The table is page-aligned and may span
    // multiple pages as Brook-specific syscall ranges grow.
    uint64_t lockStart = reinterpret_cast<uint64_t>(__syscall_table_start);
    uint64_t lockEnd   = reinterpret_cast<uint64_t>(__syscall_table_end);
    if (VmmKernelMarkReadOnly(VirtualAddress(lockStart), lockEnd - lockStart))
        SerialPrintf("SYSCALL: dispatch table locked RO at 0x%lx (%lu bytes)\n",
                     lockStart, lockEnd - lockStart);
    else
        SerialPrintf("SYSCALL: WARNING failed to lock table RO\n");
}

SyscallFn* SyscallGetTable()
{
    return g_syscallTable;
}

uint64_t SyscallGetTableAddress()
{
    return reinterpret_cast<uint64_t>(g_syscallTable);
}

// ---------------------------------------------------------------------------
// Entry point address (for LSTAR MSR)
// ---------------------------------------------------------------------------

uint64_t SyscallGetEntryPoint()
{
    return reinterpret_cast<uint64_t>(&BrookSyscallDispatcher);
}

static int64_t SyscallDispatchBody(uint64_t num, uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    Process* proc = ProcessCurrent();
    SyscallTimingCall timing(proc?proc->pid:0,proc?proc->incarnation:0,num,a0,SyscallTimingPhase::Dispatch,num==SYS_IOCTL?a1:0);
    if (proc) {
        if (const auto* filter = SeccompLoad(proc)) {
            const SeccompData data{static_cast<uint32_t>(num), SeccompX86_64,
                                   proc->syscallUserRip, {a0, a1, a2, a3, a4, a5}};
            bool audit = false;
            const uint32_t verdict = SeccompEvaluate(filter, data, &audit);
            if (audit) SeccompAudit(proc, num, verdict);
            const uint32_t action = verdict & SeccompActionMask;
            if (action != SeccompAllow && action != SeccompLog)
                return timing.Result(SeccompDenied(proc, num, verdict));
        }
    }
    if(proc && proc->addressSpace && proc->addressSpace->pagingAdmission.Exclusive()) {
        KernelOperationGuard guard(proc);
        const int error=PageableMemoryPrepareSyscall(proc,num);
        if(error)return timing.Result(error);
    }

    if (proc && proc->straceEnabled)
    {
        // If a filter is set, only trace matching categories
        if (proc->straceFilter == 0 || (SyscallCategory(num) & proc->straceFilter))
            return timing.Result(SyscallDispatchTraced(num, a0, a1, a2, a3, a4, a5));
        // Filtered out — dispatch without tracing
        SyscallFn fn2 = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
        if (!fn2) return timing.Result(-38);
        return timing.Result(fn2(a0, a1, a2, a3, a4, a5));
    }
    SyscallFn fn = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
    if (!fn) {
        SerialPrintf("[syscall] FATAL-AVOIDED: pid=%u (%s) num=%lu has null/oob entry "
                     "(a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx) -> -ENOSYS\n",
                     proc ? proc->pid : 0, proc ? proc->name : "?",
                     num, a0, a1, a2, a3, a4, a5);
        return timing.Result(-38); // -ENOSYS
    }

    // RequestServer uses the same explicitly enabled, filtered strace path
    // as every other process; no process-name-specific logging here.
    return timing.Result(fn(a0, a1, a2, a3, a4, a5));
}

static uint64_t SyscallCensusOrigin(uint64_t num)
{
    if (num >= SYSCALL_MAX) return CompatOriginOutOfRange;
    const SyscallFn fn = g_syscallTable[num];
    if (fn == sys_not_implemented) return CompatOriginUnimplemented;
    if (fn == sys_enosys_quiet) return CompatOriginQuiet;
    return CompatOriginHandler;
}

int64_t SyscallDispatchInternal(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    const int64_t result = SyscallDispatchBody(num, a0, a1, a2, a3, a4, a5);
    // Success path: one compare. rt_sigreturn returns the restored user RAX,
    // which is not a syscall result.
    if (__builtin_expect(static_cast<uint64_t>(result) >= static_cast<uint64_t>(-4095), 0) &&
        num != SYS_RT_SIGRETURN) {
        const uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
        CompatCensusObserve(num, result, args, SyscallCensusOrigin(num));
    }
    return result;
}

// ---------------------------------------------------------------------------
// SwitchToUserMode -- enter ring 3 via IRETQ (naked)
// ---------------------------------------------------------------------------

__attribute__((naked)) void SwitchToUserMode(uint64_t, uint64_t)
{
    __asm__ volatile(
        "push %%rax\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "pushfq\n\t"
        "mov %%rbp, %%gs:24\n\t"
        "mov %%rsp, %%gs:32\n\t"
        "cld\n\t"
        "pushq $0x23\n\t"
        "push %%rdi\n\t"
        "pushq $0x202\n\t"
        "pushq $0x2B\n\t"
        "push %%rsi\n\t"
        "xor %%rax, %%rax\n\t"
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%r8, %%r8\n\t"
        "xor %%r9, %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "xor %%r15, %%r15\n\t"
        "xor %%rbp, %%rbp\n\t"
        // BRO-166: clear IF before swapgs so a LAPIC timer (or any maskable
        // IRQ) cannot fire in the ring-0 window between swapgs and iretq. In
        // that window active GS is already the user base (0); a timer landing
        // here pushes a ring-0 CS, so the ISR entry stub's conditional
        // `testq $3,CS; swapgs` would SKIP swapgs and run the handler with
        // GS=0 → #PF at gs:176 (CR2=0xB0). The hand-built frame's RFLAGS=0x202
        // re-enables IF atomically on iretq to ring 3, so this only closes the
        // kernel-side window with no change to the delivered user context.
        "cli\n\t"
        "swapgs\n\t"
        "iretq\n\t"
        ::: "memory"
    );
}

} // namespace brook

// ---------------------------------------------------------------------------
// ReturnToKernel
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked)) void ReturnToKernel()
{
    __asm__ volatile(
        "mov %%gs:24, %%rbp\n\t"
        "mov %%gs:32, %%rsp\n\t"
        // BRO-207: normalize the restored kernel RFLAGS (clear NT|DF|AC) just
        // like context_switch — a resumed kernel continuation must not carry a
        // stale NT (which would #GP the next naked-handler IRETQ) or DF/AC.
        "andq $0xfffffffffffbbbff, (%%rsp)\n\t"
        "popfq\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "ret\n\t"
        ::: "memory"
    );
}
