#pragma once

#include <stdint.h>

// Returns the address of the SYSCALL entry stub (for LSTAR).
uint64_t SyscallGetEntryStub();
