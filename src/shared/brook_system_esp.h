#pragma once
/* Read-only ioctl on an open directory fd: succeeds only for the retained
 * root of the boot-admitted SSD ESP. Grants no additional write authority. */
enum { BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT = 0xb2e0,
       BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER = 0xb2e1 };
