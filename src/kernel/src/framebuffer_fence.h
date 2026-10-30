#pragma once
namespace brook {
inline void FramebufferWriteFence() { __asm__ volatile("sfence":::"memory"); }
}
