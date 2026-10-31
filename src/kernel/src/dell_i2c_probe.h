#pragma once
#include "dell_i2c_id_policy.h"
namespace brook {
// Explicit opt-in one-shot experiment. No I2C transactions or power writes.
DellI2cIdResult DellI2cIdProbe(uint32_t& value);
DellI2cIdStatus DellI2cIdProbeStatus();
}
