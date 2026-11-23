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
/* Live-store slot writes (network store updates). STORE_OPEN validates the
 * running set like ARM, then opens a USB write window over one extent of the
 * boot stick's disk that is exactly a GPT partition named BROOK_STORE_A or
 * BROOK_STORE_B (created by the operator on the host); never unallocated
 * space, never the boot volume and never the slot the running
 * /etc/live-store record names.
 * STORE_WRITE copies one chunk (<= 4 MiB) to an offset inside it, STORE_HASH
 * re-reads its first N bytes from the device and returns their SHA-256,
 * STORE_CLOSE flushes and closes the window. The running store and record are
 * untouched: a signed set update switches /etc/live-store afterwards. */
enum { BROOK_IOCTL_BOOT_STICK_STORE_OPEN = 0xb2f3, BROOK_IOCTL_BOOT_STICK_STORE_WRITE = 0xb2f4,
       BROOK_IOCTL_BOOT_STICK_STORE_HASH = 0xb2f5, BROOK_IOCTL_BOOT_STICK_STORE_CLOSE = 0xb2f6 };
typedef struct BrookStoreExtent { uint64_t first_lba, sectors; } BrookStoreExtent;
typedef struct BrookStoreWrite { uint64_t offset, data; uint32_t length, reserved; } BrookStoreWrite;
typedef struct BrookStoreHash { uint64_t bytes; uint8_t sha256[32]; } BrookStoreHash;
#define BROOK_STORE_WRITE_MAX (4u << 20)
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
       BROOK_BOOT_STICK_REFUSED_LEGACY_SET = 9, BROOK_BOOT_STICK_REFUSED_EXTENT = 10 };
