#pragma once
#include <stdint.h>

namespace brook {
struct Process;
struct WmShellSurface;
struct ChromeTrayState;
struct TrayInput;
const ChromeTrayState& ShellServiceTrayState();
void ShellServiceQueueTrayInput(const TrayInput&);
bool ShellServiceHasSession();
bool ShellServiceAllowsWindowCapture(const Process&);

void ShellServiceBeginFrame(uint64_t now);
uint64_t ShellServiceCapture(const WmShellSurface&, uint32_t width, uint32_t height,
                             uint64_t now, int32_t mx, int32_t my);
bool ShellServiceWarmLauncher(uint32_t width, uint32_t height, uint64_t now);
void ShellServiceEndCapture(uint32_t kindMask);
void ShellServiceDraw(const WmShellSurface&, uint32_t* pixels, uint32_t stride,
                      uint32_t width, uint32_t height);
// Taskbars of outputs other than the shell output (shell_output_layout.h).
// The service publishes one list per surface kind, which the shell output
// uses, so these are built and rasterised in the kernel from the same model
// builder. slot: 0..ShellServiceDetachedLimit-1, one per extra output.
// Capture returns the content key, or 0 when unavailable (draw a fallback).
// Compositor thread only.
// Without BROOK_MULTI_OUTPUT there are no extra outputs: no detached slots.
#if !defined(BROOK_MULTI_OUTPUT) || BROOK_MULTI_OUTPUT
inline constexpr uint32_t ShellServiceDetachedLimit = 3;
uint64_t ShellServiceCaptureDetached(uint32_t slot, const WmShellSurface&, uint32_t width, uint32_t height,
                                     uint64_t now, int32_t mx, int32_t my);
void ShellServiceDrawDetached(uint32_t slot, const WmShellSurface&, uint32_t* pixels, uint32_t stride,
                              uint32_t width, uint32_t height);
#else
inline constexpr uint32_t ShellServiceDetachedLimit = 0;
inline uint64_t ShellServiceCaptureDetached(uint32_t, const WmShellSurface&, uint32_t, uint32_t, uint64_t, int32_t,
                                            int32_t) { return 0; }
inline void ShellServiceDrawDetached(uint32_t, const WmShellSurface&, uint32_t*, uint32_t, uint32_t, uint32_t) {}
#endif
int64_t ShellServiceSyscallInvoke(uint64_t op, uint64_t exchange, uint64_t payload,
                                  uint64_t bytes, uint64_t reserved1, uint64_t reserved2);
}
