#pragma once
#include <stdint.h>
/* Read-only ioctl on an open directory fd: succeeds only for the retained
 * root of the boot-admitted SSD ESP. Grants no additional write authority. */
enum { BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT = 0xb2e0,
       BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER = 0xb2e1,
       BROOK_IOCTL_GET_SYSTEM_ESP_BOOT_SLOT = 0xb2e2,
       BROOK_IOCTL_CHECK_SYSTEM_ESP_GUARD_IDENTITY = 0xb2e3 };

/* BROOK_IOCTL_CHECK_SYSTEM_ESP_GUARD_IDENTITY takes a 32-byte SHA-256
 * digest of the 96 lowercase ASCII identity characters. Zero matches
 * the clean guard pair retained at guarded desktop admission. One also
 * verifies that canonical RECOVERY/BOOTSTAT.BIN is already valid and
 * exclusively pinned. No file access or write authority is granted. */
enum { BROOK_ESP_GUARD_PINNED_SLOT_HEALTH = 1 };

enum { BROOK_ESP_BOOT_SLOT_VERSION = 1,
       BROOK_ESP_BOOT_SLOT_UNTRACKED = 0,
       BROOK_ESP_BOOT_SLOT_VERIFIED = 1,
       BROOK_ESP_BOOT_SLOT_RECOVERY = 2 };

/* Loader provenance for the pair running now. The digest identifies the
 * complete selector. Nothing here authorizes staging or activation. */
typedef struct BrookSystemEspBootSlot {
    uint32_t version, mode;
    uint64_t generation;
    uint32_t slot, reserved;
    uint8_t selector_digest[32];
} BrookSystemEspBootSlot;
