// panic_probe.cpp — C++ side of the fault-tolerant panic-path memory probe.
// See panic_probe.h for rationale.  Compiled into the real kernel only; host
// tests do not link the exception path.

#include "panic_probe.h"

namespace brook {

// Bracket symbols emitted by the linker script around the .panic_extable
// section.  Each entry is a (faultRip, fixupRip) pair.
struct PanicExtableEntry {
    uint64_t faultRip;
    uint64_t fixupRip;
};

extern "C" {
extern const PanicExtableEntry __start_panic_extable[];
extern const PanicExtableEntry __stop_panic_extable[];
}

// Count of caught probe faults (see header).  Plain global — the panic path is
// single-CPU once ownership is claimed, and a relaxed increment is harmless.
static volatile uint32_t g_panicProbeFaults = 0;

bool PanicExtableFixup(uint64_t* ripInOut)
{
    uint64_t rip = *ripInOut;
    for (const PanicExtableEntry* e = __start_panic_extable;
         e < __stop_panic_extable; ++e)
    {
        if (e->faultRip == rip)
        {
            *ripInOut = e->fixupRip;
            __atomic_fetch_add(&g_panicProbeFaults, 1, __ATOMIC_RELAXED);
            return true;
        }
    }
    return false;
}

bool PanicSafeReadU64(uint64_t src, uint64_t* out)
{
    return panic_probe_u64(reinterpret_cast<const void*>(src), out) != 0;
}

size_t PanicSafeCopy(void* dst, uint64_t src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    size_t done = 0;
    // Copy 8 bytes at a time where possible; the probe reads 8 bytes, so for a
    // tail < 8 we still read a full qword from a readable address and copy only
    // the needed bytes (never reading past the requested end unless the qword is
    // readable anyway).
    while (done + 8 <= n)
    {
        uint64_t v;
        if (!PanicSafeReadU64(src + done, &v))
            return done;
        __builtin_memcpy(d + done, &v, 8);
        done += 8;
    }
    if (done < n)
    {
        uint64_t v;
        if (PanicSafeReadU64(src + done, &v))
        {
            __builtin_memcpy(d + done, &v, n - done);
            done = n;
        }
    }
    return done;
}

bool PanicAddrReadable(uint64_t src, size_t n)
{
    size_t off = 0;
    uint64_t v;
    while (off + 8 <= n)
    {
        if (!PanicSafeReadU64(src + off, &v)) return false;
        off += 8;
    }
    if (off < n)
        if (!PanicSafeReadU64(src + off, &v)) return false;
    return true;
}

uint32_t PanicProbeFaultCount()
{
    return __atomic_load_n(&g_panicProbeFaults, __ATOMIC_RELAXED);
}

}  // namespace brook
