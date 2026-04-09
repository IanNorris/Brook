#pragma once

#include <stdint.h>

// Brook device model — vtable-based, no C++ virtual dispatch.
//
// Layers:
//   Device / DeviceOps  — any device (char, block, tty)
//   BlockDevice         — specialisation with block_count / block_size
//
// Drivers allocate their Device struct statically (or from kmalloc) and
// call DeviceRegister().  The registry itself never allocates.

namespace brook {

// Maximum number of registered devices.
static constexpr uint32_t DEVICE_MAX = 64;

// Device category.
enum class DeviceType : uint8_t {
    Block = 0,  // Storage: AHCI, NVMe, virtio-blk, ramdisk
    Char  = 1,  // Keyboard, serial, framebuffer
    Tty   = 2,  // Line-discipline wrapper over a char device
};

struct Device;

// ---- Generic device operations ----
struct DeviceOps {
    // Read  len bytes from device at byte offset into buf.
    // Returns bytes read, or negative errno.
    int (*read) (Device* dev, uint64_t offset, void* buf,       uint64_t len);
    // Write len bytes to   device at byte offset from buf.
    // Returns bytes written, or negative errno.
    int (*write)(Device* dev, uint64_t offset, const void* buf, uint64_t len);
    // Device-specific control command.
    int (*ioctl)(Device* dev, uint32_t cmd, void* arg);
    // Called when all references to the device are dropped.
    void (*close)(Device* dev);
};

// ---- Block device ops (superset of DeviceOps — same layout for first 4 fields) ----
// Cast dev->ops to BlockDeviceOps* when dev->type == DeviceType::Block.
// The first four fields are identical to DeviceOps so the cast is safe.
struct BlockDeviceOps {
    // --- DeviceOps fields (must stay first and in this order) ---
    int (*read) (Device* dev, uint64_t offset, void* buf,       uint64_t len);
    int (*write)(Device* dev, uint64_t offset, const void* buf, uint64_t len);
    int (*ioctl)(Device* dev, uint32_t cmd, void* arg);
    void (*close)(Device* dev);
    // --- Block-specific ---
    uint64_t (*block_count)(Device* dev);
    uint32_t (*block_size) (Device* dev);
};

// ---- Device descriptor ----
struct Device {
    const DeviceOps* ops;   // Pointer to ops table (may be BlockDeviceOps)
    const char*      name;  // Short name: "ramdisk0", "nvme0", "kbd"
    DeviceType       type;
    void*            priv;  // Driver-private state
};

// ---- Registry ----

// Register a device.  Returns true on success; false if the table is full
// or a device with the same name is already registered.
extern "C" bool DeviceRegister(Device* dev);

// Find a device by exact name.  Returns nullptr if not found.
extern "C" Device* DeviceFind(const char* name);

// Iterate over all devices of a given type.
// Calls cb(dev, ctx) for each match.  Stops if cb returns false.
extern "C" void DeviceIterate(DeviceType type, bool (*cb)(Device* dev, void* ctx), void* ctx);

// ---- Convenience wrappers for block devices ----

// Number of logical blocks.  Caller must ensure dev->type == DeviceType::Block.
inline uint64_t DeviceBlockCount(Device* dev)
{
    return reinterpret_cast<const BlockDeviceOps*>(dev->ops)->block_count(dev);
}

// Logical block size in bytes.
inline uint32_t DeviceBlockSize(Device* dev)
{
    return reinterpret_cast<const BlockDeviceOps*>(dev->ops)->block_size(dev);
}

} // namespace brook
