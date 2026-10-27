// rtc.cpp — CMOS Real-Time Clock driver.
//
// Reads the MC146818-compatible RTC at boot to capture wall-clock time,
// then advances one epoch anchor using a single elapsed-time source.

#include "rtc.h"
#include "portio.h"
#include "serial.h"
#include "apic.h"
#include "kvmclock.h"

namespace brook {

extern volatile uint64_t g_lapicRawTickCount;  // never touched outside ISR

// Boot-time Unix epoch (seconds since 1970-01-01 00:00:00 UTC).
static uint64_t g_bootEpochSec = 0;

// Immutable after BSP boot, before application/AP clock readers start. The
// CMOS read has one-second precision; fractional time must share its epoch.
static uint64_t g_wallEpochNs = 0;
static uint64_t g_wallStarted = 0;
static bool g_wallUsesPvclock = false;

// Timezone offset in seconds east of UTC.
static int32_t g_tzOffsetSec = 0;

// Last CMOS second used by fallback timer-frequency discipline.
static uint64_t g_lastRtcSec = 0;

// Separate longer-window state for LAPIC rate correction: CMOS resolution is
// 1 second; use a ten-second observation window with a 10% deadband.  These are advanced only after we actually take
// a rate sample.
static uint64_t g_lastRateSec  = 0;
static uint64_t g_lastRateTick = 0;

// ---------------------------------------------------------------------------
// CMOS RTC access
// ---------------------------------------------------------------------------

static uint8_t CmosRead(uint8_t reg)
{
#ifdef BROOK_HOST_TEST
    extern uint8_t RtcTestCmosRead(uint8_t);
    return RtcTestCmosRead(reg);
#else
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
#endif
}

static bool CmosUpdateInProgress()
{
    return (CmosRead(0x0A) & 0x80) != 0;
}

static uint8_t BcdToBin(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

// Days in each month (non-leap).
static const uint16_t g_daysBeforeMonth[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static bool IsLeapYear(uint32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// Convert date/time to Unix epoch.
static uint64_t DateToEpoch(uint32_t year, uint32_t month, uint32_t day,
                            uint32_t hour, uint32_t minute, uint32_t second)
{
    // Days from 1970 to start of 'year'
    uint64_t days = 0;
    for (uint32_t y = 1970; y < year; y++)
    {
        days += IsLeapYear(y) ? 366 : 365;
    }
    // Days in current year up to start of 'month'
    if (month >= 1 && month <= 12)
        days += g_daysBeforeMonth[month - 1];
    // Leap day adjustment
    if (month > 2 && IsLeapYear(year))
        days += 1;
    // Day of month (1-based)
    days += day - 1;

    return days * 86400ULL + hour * 3600ULL + minute * 60ULL + second;
}

// Convert epoch to broken-down time.
static void EpochToDate(uint64_t epoch, uint32_t& year, uint32_t& month,
                        uint32_t& day, uint32_t& hour, uint32_t& minute, uint32_t& second)
{
    second = static_cast<uint32_t>(epoch % 60); epoch /= 60;
    minute = static_cast<uint32_t>(epoch % 60); epoch /= 60;
    hour   = static_cast<uint32_t>(epoch % 24); epoch /= 24;

    uint64_t totalDays = epoch;
    year = 1970;
    while (true)
    {
        uint32_t daysInYear = IsLeapYear(year) ? 366 : 365;
        if (totalDays < daysInYear) break;
        totalDays -= daysInYear;
        year++;
    }

    month = 1;
    for (uint32_t m = 1; m <= 12; m++)
    {
        uint32_t dim = (m < 12) ? (g_daysBeforeMonth[m] - g_daysBeforeMonth[m - 1])
                                : (IsLeapYear(year) ? 31 : 31); // December always 31
        if (m == 2 && IsLeapYear(year)) dim = 29;
        if (totalDays < dim) { month = m; break; }
        totalDays -= dim;
    }
    day = static_cast<uint32_t>(totalDays) + 1;
}

void RtcInit()
{
    // Wait for any update cycle to finish
    while (CmosUpdateInProgress()) {}

    // Read RTC registers (two reads to avoid mid-update race)
    uint8_t sec1, min1, hr1, day1, mon1, yr1;
    uint8_t sec2, min2, hr2, day2, mon2, yr2;
    do {
        while (CmosUpdateInProgress()) {}
        sec1 = CmosRead(0x00);
        min1 = CmosRead(0x02);
        hr1  = CmosRead(0x04);
        day1 = CmosRead(0x07);
        mon1 = CmosRead(0x08);
        yr1  = CmosRead(0x09);
        while (CmosUpdateInProgress()) {}
        sec2 = CmosRead(0x00);
        min2 = CmosRead(0x02);
        hr2  = CmosRead(0x04);
        day2 = CmosRead(0x07);
        mon2 = CmosRead(0x08);
        yr2  = CmosRead(0x09);
    } while (sec1 != sec2 || min1 != min2 || hr1 != hr2 ||
             day1 != day2 || mon1 != mon2 || yr1 != yr2);

    // Check register B for BCD vs binary mode
    uint8_t regB = CmosRead(0x0B);
    bool bcd = !(regB & 0x04);
    bool h24 = (regB & 0x02) != 0;

    uint32_t sec = bcd ? BcdToBin(sec1) : sec1;
    uint32_t min = bcd ? BcdToBin(min1) : min1;
    uint32_t hr  = bcd ? BcdToBin(hr1 & 0x7F) : (hr1 & 0x7F);
    uint32_t day = bcd ? BcdToBin(day1) : day1;
    uint32_t mon = bcd ? BcdToBin(mon1) : mon1;
    uint32_t yr  = bcd ? BcdToBin(yr1)  : yr1;

    // Handle 12-hour format
    if (!h24 && (hr1 & 0x80))
        hr = (hr == 12) ? 12 : hr + 12;
    else if (!h24 && hr == 12)
        hr = 0;

    // Century: CMOS year is 00-99, assume 2000+ for values < 70, 1900+ otherwise
    uint32_t fullYear = (yr < 70) ? 2000 + yr : 1900 + yr;

    g_bootEpochSec = DateToEpoch(fullYear, mon, day, hr, min, sec);
    g_wallUsesPvclock = KvmClockAvailable();
    g_wallStarted = g_wallUsesPvclock ? KvmClockReadNs() : ApicTickCount();
    g_wallEpochNs = g_bootEpochSec * 1000000000ULL;
    g_lastRtcSec = g_bootEpochSec;
    g_lastRateSec = 0;
    g_lastRateTick = 0;

    // Default timezone: UTC+0 (configurable via RtcSetTimezoneOffset)
    g_tzOffsetSec = 0;

    SerialPrintf("RTC: %04u-%02u-%02u %02u:%02u:%02u UTC\n",
                 fullYear, mon, day, hr, min, sec);
}

// Non-blocking CMOS snapshot: returns 0 if the RTC is mid-update or the
// two consecutive reads disagreed (meaning we caught an update).  Safe to
// call from a LAPIC ISR: only issues ~14 port reads, never spins.
static uint64_t TryReadCmosEpoch()
{
    if (CmosUpdateInProgress()) return 0;

    uint8_t sec1 = CmosRead(0x00);
    uint8_t min1 = CmosRead(0x02);
    uint8_t hr1  = CmosRead(0x04);
    uint8_t day1 = CmosRead(0x07);
    uint8_t mon1 = CmosRead(0x08);
    uint8_t yr1  = CmosRead(0x09);
    if (CmosUpdateInProgress()) return 0;
    uint8_t sec2 = CmosRead(0x00);
    if (sec1 != sec2) return 0;

    uint8_t regB = CmosRead(0x0B);
    bool bcd = !(regB & 0x04);
    bool h24 = (regB & 0x02) != 0;

    uint32_t sec = bcd ? BcdToBin(sec1) : sec1;
    uint32_t min = bcd ? BcdToBin(min1) : min1;
    uint32_t hr  = bcd ? BcdToBin(hr1 & 0x7F) : (hr1 & 0x7F);
    uint32_t day = bcd ? BcdToBin(day1) : day1;
    uint32_t mon = bcd ? BcdToBin(mon1) : mon1;
    uint32_t yr  = bcd ? BcdToBin(yr1)  : yr1;

    if (!h24 && (hr1 & 0x80))
        hr = (hr == 12) ? 12 : hr + 12;
    else if (!h24 && hr == 12)
        hr = 0;

    uint32_t fullYear = (yr < 70) ? 2000 + yr : 1900 + yr;
    return DateToEpoch(fullYear, mon, day, hr, min, sec);
}

// Coarse frequency discipline for the non-pvclock fallback only. CMOS has
// one-second resolution and may jump, so it must never rewrite an elapsed
// deadline counter. Stable-pvclock hosts keep their PIT-calibrated IRQ period.
void RtcRecalibrateLapic()
{
    uint64_t rawNow  = __atomic_load_n(&g_lapicRawTickCount, __ATOMIC_RELAXED);
    uint64_t rtcSec = TryReadCmosEpoch();
    if (rtcSec == 0) return;  // RTC busy, try again next pass

    // Only act when RTC has actually advanced — otherwise our drift
    // estimate is sub-second and unreliable.
    if (rtcSec == g_lastRtcSec) return;
    if (rtcSec < g_lastRtcSec) {
        // Clock went backwards (user changed RTC?) — resync baseline.
        g_lastRtcSec = rtcSec;
        g_lastRateSec = rtcSec;
        g_lastRateTick = rawNow;
        return;
    }

    // Use a separate, longer window to limit CMOS phase noise. Measure actual
    // delivered IRQs; never use the elapsed source or alter its epoch.
    if (g_lastRateSec == 0) {
        g_lastRateSec  = rtcSec;
        g_lastRateTick = rawNow;
    } else {
        uint64_t elapsedRtcSec = rtcSec - g_lastRateSec;
        uint64_t elapsedTicks  = rawNow - g_lastRateTick;
        if (elapsedRtcSec >= 10 && elapsedRtcSec <= 60) {
            uint32_t observedPerSec = static_cast<uint32_t>(elapsedTicks / elapsedRtcSec);
            ApicAdjustTimerRate(observedPerSec);
            g_lastRateSec  = rtcSec;
            g_lastRateTick = rawNow;
        }
    }

    g_lastRtcSec = rtcSec;
}

uint64_t RtcNowNs()
{
    const uint64_t now = g_wallUsesPvclock ? KvmClockReadNs() : ApicTickCount();
    // Never combine integer seconds from one clock with another clock's phase.
    // The millisecond elapsed API also covers VMware's virtual TSC. Raw
    // delivered IRQs are a diagnostic count, not that platform's wall clock.
    const uint64_t elapsed = now >= g_wallStarted ? now - g_wallStarted : 0;
    return g_wallEpochNs + elapsed * (g_wallUsesPvclock ? 1ULL : 1000000ULL);
}

uint64_t RtcNow()
{
    return RtcNowNs() / 1000000000ULL;
}

uint64_t RtcNowLocal()
{
    int64_t local = static_cast<int64_t>(RtcNow()) + g_tzOffsetSec;
    return (local > 0) ? static_cast<uint64_t>(local) : 0;
}

void RtcSetTimezoneOffset(int32_t offsetSec)
{
    g_tzOffsetSec = offsetSec;
    int32_t hrs = offsetSec / 3600;
    int32_t mins = (offsetSec % 3600) / 60;
    if (mins < 0) mins = -mins;
    SerialPrintf("RTC: timezone set to UTC%d:%02d\n", hrs, mins);
}

int32_t RtcGetTimezoneOffset()
{
    return g_tzOffsetSec;
}

static void U32ToStr(char* buf, uint32_t val, int width)
{
    for (int i = width - 1; i >= 0; i--)
    {
        buf[i] = static_cast<char>('0' + val % 10);
        val /= 10;
    }
}

char* RtcFormatTime(char* buf, uint64_t epochSec, bool showSeconds)
{
    uint64_t localEpoch = static_cast<uint64_t>(
        static_cast<int64_t>(epochSec) + g_tzOffsetSec);
    uint32_t y, mo, d, h, mi, s;
    EpochToDate(localEpoch, y, mo, d, h, mi, s);

    U32ToStr(buf, h, 2);
    buf[2] = ':';
    U32ToStr(buf + 3, mi, 2);
    if (showSeconds)
    {
        buf[5] = ':';
        U32ToStr(buf + 6, s, 2);
        buf[8] = '\0';
    }
    else
    {
        buf[5] = '\0';
    }
    return buf;
}

char* RtcFormatDate(char* buf, uint64_t epochSec)
{
    uint64_t localEpoch = static_cast<uint64_t>(
        static_cast<int64_t>(epochSec) + g_tzOffsetSec);
    uint32_t y, mo, d, h, mi, s;
    EpochToDate(localEpoch, y, mo, d, h, mi, s);

    U32ToStr(buf, y, 4);
    buf[4] = '-';
    U32ToStr(buf + 5, mo, 2);
    buf[7] = '-';
    U32ToStr(buf + 8, d, 2);
    buf[10] = '\0';
    return buf;
}

static const char* g_monthAbbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

char* RtcFormatTaskbar(char* buf, uint64_t epochSec, bool showSeconds)
{
    uint64_t localEpoch = static_cast<uint64_t>(
        static_cast<int64_t>(epochSec) + g_tzOffsetSec);
    uint32_t y, mo, d, h, mi, s;
    EpochToDate(localEpoch, y, mo, d, h, mi, s);

    // "HH:MM  DD Mon YYYY" or "HH:MM:SS  DD Mon YYYY"
    int pos = 0;
    U32ToStr(buf + pos, h, 2); pos += 2;
    buf[pos++] = ':';
    U32ToStr(buf + pos, mi, 2); pos += 2;
    if (showSeconds)
    {
        buf[pos++] = ':';
        U32ToStr(buf + pos, s, 2); pos += 2;
    }
    buf[pos++] = ' ';
    buf[pos++] = ' ';
    U32ToStr(buf + pos, d, 2); pos += 2;
    buf[pos++] = ' ';
    const char* monStr = (mo >= 1 && mo <= 12) ? g_monthAbbr[mo - 1] : "???";
    buf[pos++] = monStr[0];
    buf[pos++] = monStr[1];
    buf[pos++] = monStr[2];
    buf[pos++] = ' ';
    U32ToStr(buf + pos, y, 4); pos += 4;
    buf[pos] = '\0';
    return buf;
}

} // namespace brook
