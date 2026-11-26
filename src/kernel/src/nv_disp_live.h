#pragma once
// RTX 2080 display step 3, live path: `bridge hardware nv-disp ...` (see
// nv_disp_runner.h for the verbs). Off by default; nothing runs at boot.
#include "spinlock.h"

namespace brook {
// Handles "bridge hardware nv-disp <verb>"; false if the command is not ours.
bool NvDispHandle(const char* command);
// The nv-aux lock (debug_hardware.cpp): serialises nv-disp with nv-aux and dp-idle.
SpinLock* NvAuxSharedLock();
// D2: with /etc/nv-display = "1\n", bring head-0 flips up at desktop start and let the
// compositor present into them (falls back to the GOP framebuffer on any failure).
void NvDisplayDesktopStart();
// Head-0 window ARM state (ISO ctxdma handle, offset) and the flip fence: while fenced, no
// head-0 flip is submitted (damage stays pending). Both log a "fence" line with the ARM state.
struct NvHead0Arm { bool live; uint32_t state, iso, offset; };
NvHead0Arm NvDispHead0Arm();
NvHead0Arm NvDispHead0Fence(bool on, const char* why);
// NVK head-freeze diagnostics (27 Nov): one kernel-log snapshot of the display state (window
// ARM/status/LOADV, last ring kick via BAR1 vs PRAMIN, BAR1 PD3[0]). Locked = caller holds the
// nv-aux lock. nv-gsp registers its PRAMIN reader and PD3[0] address after the BAR1 map.
void NvDispSnapLocked(const char* why);
void NvDispSnap(const char* why);
void NvDispSetBar1Diag(uint32_t (*pramin)(uint64_t), uint64_t pd3Addr, uint64_t pd3Want);
// Suspend/restore dp-idle (idle-off, link watch) around a head-1 session.
void DpIdleSuspendForDisplay(bool suspend);
}
