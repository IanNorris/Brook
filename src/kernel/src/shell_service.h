#pragma once
#include <stdint.h>

namespace brook {
struct WmShellSurface;

void ShellServiceBeginFrame(uint64_t now);
uint64_t ShellServiceCapture(const WmShellSurface&, uint32_t width, uint32_t height,
                             uint64_t now, int32_t mx, int32_t my);
void ShellServiceEndCapture(uint32_t kindMask);
void ShellServiceDraw(const WmShellSurface&, uint32_t* pixels, uint32_t stride,
                      uint32_t width, uint32_t height);
int64_t ShellServiceSyscallInvoke(uint64_t op, uint64_t exchange, uint64_t payload,
                                  uint64_t bytes, uint64_t reserved1, uint64_t reserved2);
}
