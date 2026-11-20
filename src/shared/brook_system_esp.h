#pragma once
#include <stdint.h>
/* Read-only ioctl on an open directory fd: succeeds only for the retained
 * root of the boot-admitted SSD ESP. Grants no additional write authority. */
enum { BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT = 0xb2e0,
       BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER = 0xb2e1,
       BROOK_IOCTL_GET_SYSTEM_ESP_BOOT_SLOT = 0xb2e2 };

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
