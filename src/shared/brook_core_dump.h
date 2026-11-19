#pragma once

// Request a policy-managed snapshot of a live thread group for hang analysis.
// Return 0 once queued. The caller needs CAP_SYS_PTRACE in the initial user
// namespace. The kernel alone chooses and rotates the private output path.
#define BROOK_SYS_CORE_DUMP 0xB42
