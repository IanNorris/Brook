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
// Suspend/restore dp-idle (idle-off, link watch) around a head-1 session.
void DpIdleSuspendForDisplay(bool suspend);
}
