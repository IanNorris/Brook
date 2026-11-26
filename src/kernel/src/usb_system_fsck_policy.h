#pragma once
#include <stdint.h>

namespace brook {
// Boot-time repair of the admitted BROOK_SYSTEM extent (usb_system.cpp).
// The pinned static e2fsck from nixpkgs pkgsStatic.e2fsprogs 1.47.3, as
// shipped in the initrd by scripts/build_rtx_smoke_files.py. A different
// checker needs a reviewed digest here; the builder reads this line.
constexpr char UsbSystemFsckSha256[] =
    "514c26dbec14952d5e915a15f52578b0385db41403b50e8f3c00c07c7e2aab0a";
constexpr const char* UsbSystemFsckPath = "/boot/E2FSCK.ELF";
constexpr const char* UsbSystemFsckDevice = "/dev/brook-usb-system";
constexpr const char* UsbSystemFsckOptOut = "/etc/usb-system-no-fsck";
constexpr uint64_t UsbSystemFsckMaxBytes = 4ull << 20;

enum class UsbSystemFsckAction : uint8_t { Continue, Reboot, Refuse };

// e2fsck exit status is a bit mask: 1 errors corrected, 2 reboot requested,
// 4 left uncorrected, 8 operational error, 16 usage, 32 cancelled, 128
// library error. Anything but a normal exit with 0..3 keeps the refusal.
inline UsbSystemFsckAction UsbSystemFsckClassify(bool exited, int code) {
    if (!exited || code < 0 || code >= 4) return UsbSystemFsckAction::Refuse;
    return (code & 2) ? UsbSystemFsckAction::Reboot : UsbSystemFsckAction::Continue;
}

inline bool UsbSystemFsckDigestMatches(const uint8_t (&digest)[32]) {
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i)
        if (UsbSystemFsckSha256[2 * i] != hex[digest[i] >> 4] ||
            UsbSystemFsckSha256[2 * i + 1] != hex[digest[i] & 15]) return false;
    return sizeof(UsbSystemFsckSha256) == 65;
}

// A bounded copy of the checker's stdout and stderr for the boot log and
// /nix/var/log/brook after the mount. Output past the capacity is dropped
// and flagged; the serial and kernel log still receive all of it.
template <uint32_t Capacity>
struct UsbSystemFsckTranscript {
    char text[Capacity]{};
    uint32_t length = 0;
    bool truncated = false;
    void Append(const char* data, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes; ++i) {
            if (length + 1 >= Capacity) { truncated = true; return; }
            const char c = data[i];
            text[length++] = (c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? c : '?';
        }
        text[length] = 0;
    }
};
}
