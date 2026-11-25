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

// Complete all preceding writes through the device's volatile write cache.
// No argument. Returns 0 only on completion, negative errno on failure.
static constexpr uint32_t DEVICE_IOCTL_FLUSH = 0x42460001;
// Stable for the lifetime of this registered device: 1 means all aliases reject
// writes, 0 means writable, negative errno means the guarantee is unavailable.
static constexpr uint32_t DEVICE_IOCTL_READ_ONLY = 0x42460002;
// Boot-stick update gate only (boot_stick.cpp): on a read-only USB storage
// device, admit writes and flushes inside one byte range of the whole device
// while an authenticated update is active. Arg: DeviceWriteWindow*; length 0
// closes the window. READ_ONLY keeps reporting 1: no other alias may write.
struct DeviceWriteWindow { uint64_t start, length; };
static constexpr uint32_t DEVICE_IOCTL_WRITE_WINDOW = 0x42460003;
// USB system boot only. An independent, sector-aligned extent owned by
// usb_system.cpp; neither this nor the update ioctl opens the whole device.
static constexpr uint32_t DEVICE_IOCTL_USB_SYSTEM_WINDOW = 0x42460004;
// A runtime failure has fenced a previously writable filesystem backing.
// Optional, non-blocking, implemented by the USB system adapter only.
static constexpr uint32_t DEVICE_IOCTL_WRITE_FENCED = 0x42460005;
// Snapshot transport command timings; no device I/O.
struct UsbPhaseTiming { uint64_t totalNs, maxNs, lastNs; };
struct UsbStorageTiming {
    uint64_t commands, errors, dataBytes, lastBytes;
    UsbPhaseTiming command, data, status;
    uint64_t directBytes,uasFlights;
    uint32_t uasPeakDepth;
};
static constexpr uint32_t DEVICE_IOCTL_USB_STORAGE_TIMING = 0x42460006;
// Positive preferred size in bytes, or an unsupported error.
static constexpr uint32_t DEVICE_IOCTL_PREFERRED_READ_BYTES = 0x42460007;
static constexpr uint32_t DEVICE_IOCTL_USB_UNMAP_LIMITS = 0x42460008;
static constexpr uint32_t DEVICE_IOCTL_USB_UNMAP = 0x42460009;
// Internal read into exclusive, contiguous PMM pages. If retain is set
// the caller must neither read nor free the allocation until reboot.
static constexpr uint32_t DEVICE_IOCTL_USB_READ_DMA = 0x4246000a;
struct UsbDmaRead { uint64_t offset,physical;uint32_t bytes;bool retain; };
static constexpr uint32_t UsbUnmapMaxRanges=16;
struct UsbUnmapLimits { uint32_t maxSectors,maxRanges; };
struct UsbUnmapBatch { const DeviceWriteWindow* ranges;uint32_t count; };


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

// Unregister a device by pointer.  Returns true if found and removed.
// Does NOT call dev->ops->close() — caller is responsible for shutdown.
extern "C" bool DeviceUnregister(Device* dev);

// Find a device by exact name.  Returns nullptr if not found.
extern "C" Device* DeviceFind(const char* name);

// True if dev is currently present in the registry.  Does not dereference dev,
// so it is safe to use before validating a pointer recovered from another
// subsystem's cached state.
extern "C" bool DeviceIsRegistered(Device* dev);

extern "C" int DeviceFlush(Device* dev);
// All filesystem and partition block writes enter here, including modules.
extern "C" int DeviceWrite(Device* dev,uint64_t offset,const void* data,uint64_t len);
struct BlockWriteLease;
// Internal boot-only interface. The caller must establish that no cache can
// hold pending writes to these devices before installing the permanent leases.
// The caller validates ranges against device capacity. All aliases must use
// DeviceWrite; a driver's direct ops->write bypasses these reservations.
bool DeviceInstallBootWriteLeases(const BlockWriteLease* leases,uint32_t count);
int DeviceWriteLeased(Device* parent,Device* owner,uint64_t offset,const void* data,uint64_t len);
int DeviceUsbUnmapLeased(Device* parent,Device* owner,UsbUnmapBatch& batch);
// Final-sync worker only; failed closure has no side effects. No sleeping I/O
// or driver callback is executed under the admission lock.
bool DeviceCloseWrites();
bool DeviceReopenWrites();
// Only the thread which closed and drained normal write admission may publish
// clean-session completion, after its successful final filesystem sync.
bool DeviceFinalSyncOwner();

// Verify device registry integrity (canaries + checksum).  Returns true if OK.
// Logs diagnostic details on corruption.  Safe to call from timer tick context.
extern "C" bool DeviceCheckIntegrity();

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
