#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// BRO-178 catch-at-scene: name the site where the kernel GS base first goes
// user (0) while kernel code runs.
// ---------------------------------------------------------------------------
//
// The bug (BRO-178) is a SWAPGS imbalance: a ring-0 instant runs with the USER
// GS base (0) still live, so the next `movq %gs:176` (SmpCurrentCpuIndex) #PFs
// with CR2=0xB0.  It recurs on a thread's RESUME path after SchedulerBlock in a
// futex syscall, under SDL/pthread focus churn.
//
// This guard is a DIAGNOSTIC, not the fix.  Placed at the resume choke point
// (DoSwitch, right after context_switch returns) and at the head of
// SmpCurrentCpuIndex (the actual fault site — a backstop catching every caller),
// it detects the imbalance BEFORE the faulting deref, recovers a valid kernel GS
// base without ever reading gs, then reports the scene so the caller (via the
// return address) and the shadow-base value name the mechanism:
//
//   active=0, shadow=env  -> a KERNEL-entry swapgs was MISSED (kernel GS never
//                            restored on entry / restored then swapped out).
//   active=0, shadow=0     -> a DOUBLE swapgs (both bases swapped to user) or
//                            the env pointer was clobbered.
//
// Recovery-before-report is mandatory: KernelPanic()/SerialPrintf() ultimately
// read gs:176 themselves, so the report path would recursively #PF if we did
// not restore a valid kernel GS base first.
//
// FSGSBASE is NOT enabled on this kernel (CR4.FSGSBASE clear — see cpu.cpp), so
// `rdgsbase` would #UD; we read/write IA32_GS_BASE via rdmsr/wrmsr instead.
//
// Flip GS_CATCH_PANIC to 0 to LOG-and-continue (soak to enumerate a multi-site
// cluster + temporal ordering via the BSS ring buffer) instead of panicking on
// the first hit.
//
// Compiled only into the real kernel (host tests do not link KernelPanic).

#ifndef BROOK_HOST_TEST

namespace brook {

// 1 = KernelPanic on the first imbalance (one clean, fully-attributed capture).
// 0 = restore GS, record to the ring buffer, and continue (long soak).
#define GS_CATCH_PANIC 1

// Detect + (if imbalanced) recover + report.  `site` is a static string naming
// the instrumentation point; `ra` is the caller's return address (pass
// __builtin_return_address(0)) so a backstop hit names the true caller.
//
// GS-free and re-entrancy-safe: it must never call anything that reads gs:
// (ThisCpu/SmpCurrentCpuIndex, locks, gs-relative logging) before recovery.
__attribute__((no_instrument_function))
void GsBaseCatchAtScene(const char* site, void* ra);

}  // namespace brook

// One-liner for call sites.  Costs one rdmsr + a predicted-taken branch on the
// hot path (GS already kernel-side).
#define GS_CATCH_SCENE(site) \
    ::brook::GsBaseCatchAtScene((site), __builtin_return_address(0))

#else  // BROOK_HOST_TEST

#define GS_CATCH_SCENE(site) ((void)0)

#endif  // BROOK_HOST_TEST
