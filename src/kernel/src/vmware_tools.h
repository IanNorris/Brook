#pragma once
#include <stdint.h>

// Guest side of the VMware Tools "toolbox" channel: tools version and soft
// power capabilities, guest IP, Shut Down/Restart Guest, and host resize
// hints for a display driver. Everything is inert unless exact VMware CPUID
// detection succeeds.
namespace brook {
void VmwareToolsStart();
}

// Display driver interface (module ABI). Register enables host resize hints
// and returns false when the tools service is not running. Take/Finish run in
// the display poll context; Take returns a pending request at most until its
// deadline and Finish(applied) answers it. A retry leaves it pending.
extern "C" bool VmwareToolsRegisterResolution(uint32_t maxWidth, uint32_t maxHeight);
extern "C" bool VmwareToolsTakeResolution(uint32_t* width, uint32_t* height);
extern "C" void VmwareToolsFinishResolution(bool applied);
