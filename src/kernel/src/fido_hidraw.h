#pragma once

#include <stdint.h>

namespace brook {

struct Tmpfs;
struct FdEntry;
struct Process;
struct VnodeStat;
struct HidrawFile;

inline constexpr unsigned HidrawReportBytes = 64;
inline constexpr unsigned HidrawDescriptorLimit = 256;

// The transfer layer owns the cookie for the life of the module. QueueOutput
// is called outside the hidraw lock and must check the exact device identity
// before submitting DMA. The driver can feed reports from interrupt context.
using HidrawQueueOutput = int (*)(void* cookie, uint32_t identity, const uint8_t* report);
extern "C" uint32_t HidrawAttach(uint16_t vendor, uint16_t product,
                                 const uint8_t* desc, unsigned size,
                                 HidrawQueueOutput output, void* cookie);
extern "C" void HidrawActivate(uint32_t identity);
extern "C" void HidrawDetach(uint32_t identity);
extern "C" void HidrawReceive(uint32_t identity, const uint8_t* report);
extern "C" void HidrawOutputComplete(uint32_t identity, bool ok);
extern "C" void HidrawTransportFailed(uint32_t identity);

void HidrawMount(Tmpfs*);
bool HidrawDeviceNumber(uint64_t rdev);
int HidrawOpen(Process*, const VnodeStat&, uint32_t flags);
void HidrawRef(HidrawFile*);
void HidrawUnref(HidrawFile*);
uint32_t HidrawReady(HidrawFile*, uint32_t events);
void HidrawStat(HidrawFile*, VnodeStat&);
int64_t HidrawRead(FdEntry*, uint64_t buffer, uint64_t bytes);
int64_t HidrawWrite(FdEntry*, uint64_t buffer, uint64_t bytes, bool user);
int64_t HidrawIoctl(HidrawFile*, uint64_t cmd, uint64_t arg);

} // namespace brook
