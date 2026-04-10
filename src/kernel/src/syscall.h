#pragma once

#include <stdint.h>

namespace brook {

// Maximum number of syscalls. Must match the dispatcher bounds check.
static constexpr uint64_t SYSCALL_MAX = 400;

// Syscall numbers (Linux-compatible where practical)
static constexpr uint64_t SYS_READ      = 0;
static constexpr uint64_t SYS_WRITE     = 1;
static constexpr uint64_t SYS_EXIT      = 60;

// Syscall function type — same signature as Linux: returns int64_t,
// up to 6 arguments via rdi, rsi, rdx, r10→rcx, r8, r9.
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
