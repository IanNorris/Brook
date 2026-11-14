#pragma once
// Boot timing arithmetic, ACPI FPDT parsing and text formatting. Pure code:
// no hardware access, allocation or locking, so host tests can cover it and
// the compositor can format without the potentially locking printf.
#include <stddef.h>
#include <stdint.h>
#include "boot_protocol/boot_protocol.h"

namespace brook {

// Median of five PIT-window TSC rate samples. Zero when the median is outside
// 1 MHz..20 GHz; the median tolerates two disturbed windows (VM steal).
inline uint32_t BootTimingMedianKhz(const uint32_t (&samples)[5])
{
    uint32_t s[5];
    for (unsigned i = 0; i < 5; ++i) s[i] = samples[i];
    for (unsigned i = 1; i < 5; ++i) {
        const uint32_t x = s[i];unsigned j = i;
        while (j && s[j - 1] > x) {s[j] = s[j - 1];--j;}
        s[j] = x;
    }
    return s[2] >= 1000 && s[2] <= 20000000 ? s[2] : 0;
}

// Elapsed whole milliseconds between two TSC values; false for a reversed
// span or missing frequency. No intermediate overflow for any 64-bit delta.
inline bool BootTscToMs(uint64_t from, uint64_t to, uint32_t khz, uint64_t& ms)
{
    if (!khz || to < from) return false;
    ms = (to - from) / khz;
    return true;
}

// ACPI FPDT Firmware Basic Boot Performance Data Record (values in ns since
// the platform timer's zero, normally the reset).
struct FpdtBootRecord {
    bool valid = false;
    uint64_t resetEnd = 0, loadImageStart = 0, startImageStart = 0;
    uint64_t exitBootServicesEntry = 0, exitBootServicesExit = 0;
};

inline uint16_t BootLe16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t BootLe32(const uint8_t* p) { return uint32_t(BootLe16(p)) | (uint32_t(BootLe16(p + 2)) << 16); }
inline uint64_t BootLe64(const uint8_t* p) { return uint64_t(BootLe32(p)) | (uint64_t(BootLe32(p + 4)) << 32); }

// `fpdt` is the complete, checksum-validated table (36-byte ACPI header then
// performance records). Returns the FBPT physical address from the Basic Boot
// Performance Table Pointer Record (type 0, length 16), or 0.
inline uint64_t BootFpdtFbptAddress(const uint8_t* fpdt, uint32_t length)
{
    if (!fpdt || length < 36 || fpdt[0] != 'F' || fpdt[1] != 'P' || fpdt[2] != 'D' || fpdt[3] != 'T') return 0;
    if (BootLe32(fpdt + 4) != length) return 0;
    for (uint32_t at = 36; length - at >= 4;) {
        const uint16_t type = BootLe16(fpdt + at);
        const uint8_t recordLength = fpdt[at + 2];
        if (recordLength < 4 || recordLength > length - at) return 0;
        if (type == 0 && recordLength == 16) return BootLe64(fpdt + at + 8);
        at += recordLength;
    }
    return 0;
}

// Parse an FBPT ("FBPT", u32 length, then records) for the type-2 record
// (length 48). Rejects all-zero or non-monotonic timestamps.
inline bool BootParseFbpt(const uint8_t* fbpt, uint32_t available, FpdtBootRecord& out)
{
    out = {};
    if (!fbpt || available < 8 || fbpt[0] != 'F' || fbpt[1] != 'B' || fbpt[2] != 'P' || fbpt[3] != 'T') return false;
    const uint32_t length = BootLe32(fbpt + 4);
    if (length < 8 || length > available) return false;
    for (uint32_t at = 8; length - at >= 4;) {
        const uint16_t type = BootLe16(fbpt + at);
        const uint8_t recordLength = fbpt[at + 2];
        if (recordLength < 4 || recordLength > length - at) return false;
        if (type == 2 && recordLength >= 48) {
            const uint8_t* r = fbpt + at + 8;
            FpdtBootRecord v{};
            v.resetEnd = BootLe64(r);v.loadImageStart = BootLe64(r + 8);v.startImageStart = BootLe64(r + 16);
            v.exitBootServicesEntry = BootLe64(r + 24);v.exitBootServicesExit = BootLe64(r + 32);
            if (!v.startImageStart || v.startImageStart < v.resetEnd) return false;
            if (v.exitBootServicesEntry && v.exitBootServicesEntry < v.startImageStart) return false;
            if (v.exitBootServicesExit && v.exitBootServicesExit < v.exitBootServicesEntry) return false;
            v.valid = true;out = v;return true;
        }
        at += recordLength;
    }
    return false;
}

struct BootTimingInputs {
    uint32_t protocolVersion = 0;
    bool loaderTimingPresent = false; // BootHasTiming(): extension marker set
    BootTiming loader{};            // zero for a loader without the extension
    uint64_t kernelEntryTsc = 0, desktopReadyTsc = 0;
    uint32_t tscKhz = 0;
    FpdtBootRecord fpdt{};
    // Used only when the TSC rate is unknown (the previous toast clock).
    bool fallbackKernelValid = false;
    uint64_t fallbackKernelMs = 0;
};

struct BootTimingValue { bool valid = false; uint64_t ms = 0; };
struct BootTimingSummary {
    BootTimingValue firmware, wait, loader, kernel, total;
    BootTimingValue tscFirmware, fpdtFirmware; // both candidates, for the log
    const char* firmwareSource = "none";       // "fpdt", "tsc" or "none"
    bool loaderStamps = false;                 // loader timing extension present
};

// TSC-since-reset is only meaningful after a clean reset; anything above ten
// minutes (or a stamp not before the kernel's own) is treated as unknown.
constexpr uint64_t BootFirmwarePlausibleMs = 10 * 60 * 1000;

inline BootTimingSummary BootTimingCompute(const BootTimingInputs& in)
{
    BootTimingSummary s{};
    const uint32_t khz = in.tscKhz;
    const BootTiming& t = in.loader;
    s.loaderStamps = in.loaderTimingPresent && (t.flags & BootTimingValid) && t.loaderEntryTsc &&
        t.loaderEntryTsc <= in.kernelEntryTsc && (!t.kernelJumpTsc || t.kernelJumpTsc <= in.kernelEntryTsc);
    uint64_t ms = 0;
    if (BootTscToMs(in.kernelEntryTsc, in.desktopReadyTsc, khz, ms) && in.kernelEntryTsc) s.kernel = {true, ms};
    else if (in.fallbackKernelValid) s.kernel = {true, in.fallbackKernelMs};
    if (s.loaderStamps && khz) {
        uint64_t waitTsc = 0;
        if (t.flags & BootTimingWaitRan) {
            if (t.waitStartTsc && t.waitEndTsc >= t.waitStartTsc && t.waitStartTsc >= t.loaderEntryTsc &&
                t.waitEndTsc <= in.kernelEntryTsc) waitTsc = t.waitEndTsc - t.waitStartTsc;
            else waitTsc = ~uint64_t(0);
        }
        if (waitTsc != ~uint64_t(0)) {
            s.wait = {true, waitTsc / khz};
            // The loader bucket ends at kernel entry so the buckets add up.
            s.loader = {true, (in.kernelEntryTsc - t.loaderEntryTsc - waitTsc) / khz};
        }
        const uint64_t sinceReset = t.loaderEntryTsc / khz;
        if (sinceReset < BootFirmwarePlausibleMs) s.tscFirmware = {true, sinceReset};
    }
    if (in.fpdt.valid && in.fpdt.startImageStart / 1000000 < BootFirmwarePlausibleMs)
        s.fpdtFirmware = {true, in.fpdt.startImageStart / 1000000};
    if (s.fpdtFirmware.valid) {s.firmware = s.fpdtFirmware;s.firmwareSource = "fpdt";}
    else if (s.tscFirmware.valid) {s.firmware = s.tscFirmware;s.firmwareSource = "tsc";}
    if (s.firmware.valid && s.wait.valid && s.loader.valid && s.kernel.valid && khz)
        s.total = {true, s.firmware.ms + s.wait.ms + s.loader.ms + s.kernel.ms};
    return s;
}

// Bounded string builder: always NUL-terminated, silently truncates.
struct BootText {
    char* out;size_t cap;size_t used = 0;
    BootText(char* o, size_t c) : out(o), cap(c) { if (cap) out[0] = 0; }
    void Add(const char* v) { while (v && *v && used + 1 < cap) out[used++] = *v++; if (cap) out[used] = 0; }
    void Number(uint64_t v) {
        char rev[21];unsigned n = 0;
        do {rev[n++] = char('0' + v % 10);v /= 10;} while (v);
        char buf[21];unsigned k = 0;while (n) buf[k++] = rev[--n];buf[k] = 0;Add(buf);
    }
    // Seconds with `digits` (1 or 2) fractional digits, rounded to nearest.
    void Seconds(uint64_t ms, unsigned digits) {
        const uint64_t unit = digits >= 2 ? 10 : 100, scaled = (ms + unit / 2) / unit;
        const uint64_t div = digits >= 2 ? 100 : 10;
        Number(scaled / div);Add(".");
        if (digits >= 2) {char d[3] = {char('0' + scaled / 10 % 10), char('0' + scaled % 10), 0};Add(d);}
        else {char d[2] = {char('0' + scaled % 10), 0};Add(d);}
    }
    void Value(const char* label, const BootTimingValue& v) {
        Add(label);Add(" ");
        if (v.valid) {Seconds(v.ms, 1);Add(" s");} else Add("n/a");
    }
    void Ms(const char* key, const BootTimingValue& v) {
        Add(key);Add("=");if (v.valid) Number(v.ms); else Add("na");
    }
};

// Toast headline, e.g. "Desktop ready in 8.12 s from power-on".
inline void BootTimingHeadline(char* out, size_t cap, const BootTimingSummary& s)
{
    BootText t(out, cap);
    t.Add("Desktop ready in ");
    if (s.total.valid) {t.Seconds(s.total.ms, 2);t.Add(" s from power-on");}
    else if (s.kernel.valid) {t.Seconds(s.kernel.ms, 2);t.Add(" s from kernel start");}
    else t.Add("n/a");
}

// Toast breakdown. foldLoader moves the loader into the kernel bucket for a
// narrow toast ("Kernel" then covers loader + kernel).
inline void BootTimingRow(char* out, size_t cap, const BootTimingSummary& s, bool foldLoader)
{
    BootText t(out, cap);
    t.Value("Firmware", s.firmware);t.Add(" | ");
    t.Value("Menu", s.wait);t.Add(" | ");
    if (foldLoader) {
        if (s.loader.valid && s.kernel.valid) t.Value("Loader+kernel", {true, s.loader.ms + s.kernel.ms});
        else t.Value("Kernel", s.kernel);
        return;
    }
    t.Value("Loader", s.loader);t.Add(" | ");
    t.Value("Kernel", s.kernel);
}

// One kernel-log line (< 160 characters, the debug ring's line width).
inline void BootTimingLogLine(char* out, size_t cap, const BootTimingSummary& s, const BootTimingInputs& in)
{
    BootText t(out, cap);
    t.Add("BOOT_TIMING ");t.Ms("firmware_ms", s.firmware);
    t.Add(" src=");t.Add(s.firmwareSource);
    t.Add(" ");t.Ms("wait_ms", s.wait);
    t.Add(" ");t.Ms("loader_ms", s.loader);
    t.Add(" ");t.Ms("kernel_ms", s.kernel);
    t.Add(" ");t.Ms("total_ms", s.total);
    t.Add(" tsc_khz=");t.Number(in.tscKhz);
    t.Add(" proto=");t.Number(in.protocolVersion);
    t.Add(" ext=");t.Number(in.loaderTimingPresent ? 1 : 0);
}

// Supporting detail: both firmware candidates and the FPDT exit timestamp.
inline void BootTimingDetailLine(char* out, size_t cap, const BootTimingSummary& s, const BootTimingInputs& in)
{
    BootText t(out, cap);
    t.Add("BOOT_TIMING_DETAIL ");t.Ms("tsc_fw_ms", s.tscFirmware);
    t.Add(" ");t.Ms("fpdt_fw_ms", s.fpdtFirmware);
    t.Add(" fpdt_reset_ms=");if (in.fpdt.valid) t.Number(in.fpdt.resetEnd / 1000000); else t.Add("na");
    t.Add(" fpdt_ebs_ms=");
    if (in.fpdt.valid && in.fpdt.exitBootServicesEntry) t.Number(in.fpdt.exitBootServicesEntry / 1000000); else t.Add("na");
    t.Add(" flags=0x");
    const char hex[] = "0123456789abcdef";char f[2] = {hex[in.loader.flags & 15], 0};t.Add(f);
    t.Add(" wait_cfg_ms=");t.Number(in.loader.waitConfiguredMs);
    t.Add(" wait_keys=");t.Number(in.loader.waitKeyCount);
    t.Add(" last_key=0x");
    for (int shift = 28; shift >= 0; shift -= 4) {char d[2] = {hex[(in.loader.waitLastKey >> shift) & 15], 0};t.Add(d);}
}

} // namespace brook
