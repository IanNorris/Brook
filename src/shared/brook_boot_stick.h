#pragma once
#include <stdint.h>
/* Boot-stick update gate (RAM-only smoke stick). Any fd; CAP_SYS_ADMIN;
 * requires /etc/boot-stick-update in the initrd and a STICK_SETS loader that
 * reported the boot volume. ARM mounts exactly that FAT volume at
 * /media/bootstick after checking: GPT partition GUID/entry/extent equal the
 * loader's device path, FAT32 label BROOK_RTX, and the kernel+initrd files of
 * the set the loader booted (root or PREV/) hash to what the loader verified
 * and to the kernel's own initrd digest. Mode RW also opens a USB write window
 * limited to that partition; every other device stays read-only. DISARM
 * flushes, closes the window and unmounts. */
enum { BROOK_IOCTL_BOOT_STICK_ARM = 0xb2f0, BROOK_IOCTL_BOOT_STICK_DISARM = 0xb2f1,
       BROOK_IOCTL_BOOT_STICK_STATUS = 0xb2f2 };
enum { BROOK_BOOT_STICK_RO = 1, BROOK_BOOT_STICK_RW = 2 };
#define BROOK_BOOT_STICK_MOUNT "/media/bootstick"
typedef struct BrookBootStickStatus {
    uint32_t version;        /* 1 */
    uint32_t armed;          /* 0, BROOK_BOOT_STICK_RO or _RW */
    uint32_t set;            /* loader: 0 legacy, 1 root, 2 PREV */
    uint32_t trial;          /* loader: BROOK_STICK_TRIAL_* */
    int32_t  last_refusal;   /* 0 or a BROOK_BOOT_STICK_REFUSED_* code */
    uint32_t running_location; /* set of files == running pair at the last ARM: 1 root, 2 PREV */
    uint8_t  partition_guid[16];
    uint8_t  kernel_sha256[32];
    uint8_t  initrd_sha256[32];
} BrookBootStickStatus;
enum { BROOK_BOOT_STICK_REFUSED_DISABLED = 1, BROOK_BOOT_STICK_REFUSED_NO_LOADER_VOLUME = 2,
       BROOK_BOOT_STICK_REFUSED_NO_DEVICE = 3, BROOK_BOOT_STICK_REFUSED_AMBIGUOUS = 4,
       BROOK_BOOT_STICK_REFUSED_LABEL = 5, BROOK_BOOT_STICK_REFUSED_PAYLOAD = 6,
       BROOK_BOOT_STICK_REFUSED_BUSY = 7, BROOK_BOOT_STICK_REFUSED_MOUNT = 8,
       BROOK_BOOT_STICK_REFUSED_LEGACY_SET = 9 };
