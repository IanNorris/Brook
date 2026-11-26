#pragma once
#include <stdint.h>

namespace brook {
struct Device; struct Process; struct Vnode;

// The single Linux-ABI route to a block device in a USB-system boot:
// /dev/brook-usb-system, a view of the admitted BROOK_SYSTEM adapter only.
// It exists for one tracked e2fsck child while its session is active. Every
// open and transfer rechecks that the caller is that child and the session
// is still active, so the view is useless once the session ends. One session
// per boot; it is never reopened.
bool UsbSystemFsckSessionBegin(Device* adapter, const Process* child);
Vnode* UsbSystemFsckOpenLinux(uint64_t flags, int* error);
// Revokes the view. True when no descriptor or transfer remains.
bool UsbSystemFsckSessionEnd();
// The checker's stdout/stderr (DebugConsole writes) while the session is
// active; ignored for any other writer.
void UsbSystemFsckCapture(const Process* writer, const char* text, uint32_t bytes);
const char* UsbSystemFsckTranscriptText(uint32_t* length, bool* truncated);
}
