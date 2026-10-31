#pragma once
#include "dell_audio_firmware.h"
namespace brook {
// Audited opt-in diagnostic. Never reads device registers or alters power.
bool DellAudioProbeRead(DellAudioFirmwareSnapshot& out,const char*& reason);
}
