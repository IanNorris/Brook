// syscall.cpp -- SYSCALL/SYSRET dispatcher, syscall table, and user-mode entry.
//
// Dispatcher adapted from Enkel OS (IanNorris/Enkel, MIT license).
//
// SYSCALL ABI (x86-64):
//   RAX = syscall number
//   RDI, RSI, RDX, R10, R8, R9 = arguments (R10 replaces RCX)
//   RCX = user RIP (saved by SYSCALL instruction)
//   R11 = user RFLAGS (saved by SYSCALL instruction)
//   RAX = return value
//
// KernelCpuEnv layout (accessed via gs: after SWAPGS):
//   [gs:0]  = selfPtr
//   [gs:8]  = syscallStack  (kernel stack top)
//   [gs:16] = syscallTable  (pointer to SyscallFn[SYSCALL_MAX])
//   [gs:24] = kernelRbp     (saved by SwitchToUserMode)
//   [gs:32] = kernelRsp     (saved by SwitchToUserMode)
//   [gs:40] = currentPid

#include "syscall.h"
#include "cpu.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"

// Forward declaration -- ReturnToKernel has C linkage, defined below.
extern "C" __attribute__((naked)) void ReturnToKernel();

// ---------------------------------------------------------------------------
// SYSCALL dispatcher -- naked assembly, pointed to by LSTAR MSR.
// Must be outside namespace for clean C linkage.
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked, used)) void BrookSyscallDispatcher()
{
    __asm__ volatile(
        // Save user RIP (SYSCALL stores next RIP in RCX) onto user stack.
        "push %%rcx\n\t"

        // Switch to kernel GS base
        "swapgs\n\t"

        // Save user RSP, switch to kernel syscall stack
        "mov %%rsp, %%rcx\n\t"
        "mov %%gs:8, %%rsp\n\t"
        "and $~0xF, %%rsp\n\t"

        // Build stack frame with user state
        "push %%rcx\n\t"            // user RSP
        "push %%r11\n\t"            // user RFLAGS
        "push %%rbp\n\t"
        "mov %%rsp, %%rbp\n\t"

        // Save registers
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"

        // R10 -> RCX (4th arg, System V ABI)
        "mov %%r10, %%rcx\n\t"

        // Bounds check
        "cmp $400, %%rax\n\t"
        "jae .Lsyscall_invalid\n\t"

        // Dispatch: handler = syscallTable[rax]
        "mov %%gs:16, %%r12\n\t"
        "call *(%%r12, %%rax, 8)\n\t"
        "jmp .Lsyscall_return\n\t"

        // Invalid syscall number
        ".Lsyscall_invalid:\n\t"
        "mov $-38, %%rax\n\t"

        // Restore and return to user mode
        ".Lsyscall_return:\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"

        "pop %%rbp\n\t"
        "popfq\n\t"
        "pop %%rsp\n\t"

        // Switch back to user GS
        "swapgs\n\t"

        // Restore user RIP and SYSRET
        "pop %%rcx\n\t"
        ".byte 0x48\n\t"
        "sysret\n\t"
        ::: "memory"
    );
}

namespace brook {

// ---------------------------------------------------------------------------
// Syscall handlers
// ---------------------------------------------------------------------------

static int64_t sys_write(uint64_t fd, uint64_t bufAddr, uint64_t count,
                          uint64_t, uint64_t, uint64_t)
{
    if (fd != 1 && fd != 2) return -9; // EBADF
    const char* buf = reinterpret_cast<const char*>(bufAddr);
    for (uint64_t i = 0; i < count; ++i)
        SerialPutChar(buf[i]);
    return static_cast<int64_t>(count);
}

static int64_t sys_exit(uint64_t status, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    SerialPrintf("sys_exit: process exited with status %lu\n", status);
    ReturnToKernel();
    return 0; // not reached
}

static int64_t sys_not_implemented(uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    return -38; // ENOSYS
}

// ---------------------------------------------------------------------------
// Syscall table
// ---------------------------------------------------------------------------

static SyscallFn g_syscallTable[SYSCALL_MAX];

void SyscallTableInit()
{
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        g_syscallTable[i] = sys_not_implemented;

    g_syscallTable[SYS_WRITE] = sys_write;
    g_syscallTable[SYS_EXIT]  = sys_exit;

    SerialPrintf("SYSCALL: table initialised (%u entries, %u implemented)\n",
                 static_cast<unsigned>(SYSCALL_MAX), 2);
}

SyscallFn* SyscallGetTable()
{
    return g_syscallTable;
}

// ---------------------------------------------------------------------------
// Entry point address (for LSTAR MSR)
// ---------------------------------------------------------------------------

uint64_t SyscallGetEntryPoint()
{
    return reinterpret_cast<uint64_t>(&BrookSyscallDispatcher);
}

// ---------------------------------------------------------------------------
// SwitchToUserMode -- enter ring 3 via IRETQ (naked)
// ---------------------------------------------------------------------------
// SysV ABI: rdi = userStack, rsi = userEntry

__attribute__((naked)) void SwitchToUserMode(uint64_t, uint64_t)
{
    __asm__ volatile(
        // rdi = userStack, rsi = userEntry

        // Save all kernel registers (restored by ReturnToKernel)
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

        // Save kernel RBP and RSP to gs:24 and gs:32
        "mov %%rbp, %%gs:24\n\t"
        "mov %%rsp, %%gs:32\n\t"

        "cld\n\t"

        // Build IRETQ frame: SS, RSP, RFLAGS, CS, RIP
        "pushq $0x23\n\t"           // SS = GDT_USER_DATA (0x20|3)
        "push %%rdi\n\t"            // user RSP
        "pushq $0x202\n\t"          // RFLAGS: IF=1
        "pushq $0x2B\n\t"           // CS = GDT_USER_CODE (0x28|3)
        "push %%rsi\n\t"            // user RIP

        // Zero GPRs to prevent info leaks
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

        // Switch to user GS base
        "swapgs\n\t"

        "iretq\n\t"
        ::: "memory"
    );
}

} // namespace brook

// ---------------------------------------------------------------------------
// ReturnToKernel -- restore kernel context after sys_exit (naked, C linkage)
// ---------------------------------------------------------------------------
// Called from the SYSCALL dispatcher context (already on kernel GS).

extern "C" __attribute__((naked)) void ReturnToKernel()
{
    __asm__ volatile(
        // Already on kernel GS (dispatcher did SWAPGS on entry)
        "mov %%gs:24, %%rbp\n\t"
        "mov %%gs:32, %%rsp\n\t"

        // Pop registers SwitchToUserMode pushed (reverse order)
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

        // Return to caller of SwitchToUserMode
        "ret\n\t"
        ::: "memory"
    );
}
