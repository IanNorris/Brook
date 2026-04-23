#pragma once

#include <stdint.h>

namespace brook {

// Read the CMOS RTC and initialise the wall-clock epoch.
// Must be called early in boot (before LAPIC timer starts).
void RtcInit();

// Re-align g_lapicTickCount against the CMOS RTC.  Called once per second
// from the BSP's LAPIC timer ISR to correct for timer calibration drift.
void RtcRecalibrateLapic();

// Get current wall-clock time as Unix timestamp (UTC).
uint64_t RtcNow();

// Get current wall-clock time adjusted by timezone offset.
uint64_t RtcNowLocal();

// Set timezone offset in seconds east of UTC (e.g. +3600 for UTC+1).
void RtcSetTimezoneOffset(int32_t offsetSec);

// Get current timezone offset.
int32_t RtcGetTimezoneOffset();

// Format a Unix timestamp into "HH:MM" or "HH:MM:SS" (local time).
// Returns buf. If showSeconds is false, buf needs at least 6 bytes.
// If showSeconds is true, buf needs at least 9 bytes.
char* RtcFormatTime(char* buf, uint64_t epochSec, bool showSeconds);

// Format a Unix timestamp into "YYYY-MM-DD" (local time). buf needs 11 bytes.
char* RtcFormatDate(char* buf, uint64_t epochSec);

// Format as "HH:MM  DD Mon YYYY" for taskbar. buf needs 24 bytes.
char* RtcFormatTaskbar(char* buf, uint64_t epochSec, bool showSeconds);

} // namespace brook
