#pragma once

#include <stdint.h>
#include <stddef.h>

// Fault-tolerant memory access for the panic / exception-dump path.
//
// Every pointer the panic handler dereferences (RBP-chain frames, process
// structs, raw stack bytes, corrupt-page-table frames) may be unmapped,
// non-canonical, or point through corrupt paging structures.  A blind
// dereference #PFs/#GPs the handler and nests the fault — the "no QR, CPU
// pegged" livelock.  Route every such read through PanicSafe* so a bad address
// yields false instead of a nested fault.
//
// Backed by panic_probe_u64 (panic_probe.S) + the .panic_extable fixup checked
// at the top of HandleExceptionFull (idt.cpp).

namespace brook {

// Look up a faulting RIP in the immutable .panic_extable.  If found, rewrite
// *ripInOut to the fixup target and return true (the handler then IRETQs to the
// fixup, which returns false to the probe's caller).  GS-free, allocation-free,
// lock-free — safe as the very first action of the fault handler.
bool PanicExtableFixup(uint64_t* ripInOut);

// Safely load 8 bytes from src.  Returns true and writes *out on success; false
// (out untouched) if src faulted.  src need not be aligned for correctness, but
// callers walking frames should pass 8-aligned addresses.
bool PanicSafeReadU64(uint64_t src, uint64_t* out);

// Safely copy n bytes from src into the trusted buffer dst.  Stops at the first
// unreadable byte; returns the number of bytes successfully copied (which may be
// less than n).  Never faults the handler.
size_t PanicSafeCopy(void* dst, uint64_t src, size_t n);

// True if src..src+n is fully readable (probes it via PanicSafeReadU64).
bool PanicAddrReadable(uint64_t src, size_t n);

// Total number of probe faults caught this panic.  A poison-storm could
// otherwise burn the dying machine's time; callers may consult this against a
// budget.  Reset is not needed (the machine halts after one panic).
uint32_t PanicProbeFaultCount();

}  // namespace brook

extern "C" {
// Assembly primitive (panic_probe.S).  Returns 1 on success, 0 on fault.
int panic_probe_u64(const void* src, uint64_t* dst);
}
