#pragma once
#include <stdint.h>
namespace brook {
enum class AhciSystemBoot { Absent, Ready, Maintenance };
// Boot only. An existing selector which fails at any stage is Maintenance.
AhciSystemBoot AhciSystemVolumeMount(uint8_t* nextDrive);
}
