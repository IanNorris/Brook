#pragma once
#include <stdint.h>
namespace brook {
// VMware's reliable virtual TSC contract, conservatively gated on invariant
// TSC and RDTSCP. This is NOT KVM pvclock, a vDSO page, or steal accounting.
bool VmwareClockInit();
uint32_t VmwareClockTscKhz();
bool VmwareClockReadTicks(uint64_t* ticks);
}
