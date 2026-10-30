#include "ramdisk.h"
#include "memory/heap.h"
#include "serial.h"
#include "string.h"

namespace brook {

// ---- DeviceOps implementation ----

static int RamdiskRead(Device* dev, uint64_t offset, void* buf, uint64_t len)
{
    auto* p = static_cast<RamdiskPriv*>(dev->priv);
    if (offset >= p->size) return 0;
    if (len > p->size - offset) len = p->size - offset;
    if (len > 0x7fffffffu) len = 0x7fffffffu;

    const uint8_t* src = p->data + offset;
    uint8_t*       dst = static_cast<uint8_t*>(buf);
    memcpy(dst, src, len);
    return static_cast<int>(len);
}

static int RamdiskWrite(Device* dev, uint64_t offset, const void* buf, uint64_t len)
{
    auto* p = static_cast<RamdiskPriv*>(dev->priv);
    if (offset >= p->size) return 0;
    if (len > p->size - offset) len = p->size - offset;
    if (len > 0x7fffffffu) len = 0x7fffffffu;

    const uint8_t* src = static_cast<const uint8_t*>(buf);
    uint8_t*       dst = p->data + offset;
    memcpy(dst, src, len);
    return static_cast<int>(len);
}

static int RamdiskIoctl(Device* /*dev*/, uint32_t cmd, void* /*arg*/)
{
    // RAM is the backing store; writes have no separate pending cache.
    if (cmd == DEVICE_IOCTL_READ_ONLY) return 0;
    return cmd == DEVICE_IOCTL_FLUSH ? 0 : -95;
}

static void RamdiskClose(Device* /*dev*/) {}

static uint64_t RamdiskBlockCount(Device* dev)
{
    auto* p = static_cast<RamdiskPriv*>(dev->priv);
    return p->size / p->blockSize;
}

static uint32_t RamdiskBlockSize(Device* dev)
{
    auto* p = static_cast<RamdiskPriv*>(dev->priv);
    return p->blockSize;
}

// ---- Static ops table ----

static const BlockDeviceOps g_ramdiskOps = {
    .read        = RamdiskRead,
    .write       = RamdiskWrite,
    .ioctl       = RamdiskIoctl,
    .close       = RamdiskClose,
    .block_count = RamdiskBlockCount,
    .block_size  = RamdiskBlockSize,
};

// A distinct constant operations table makes the read-only promise permanent;
// no runtime flag can turn an immutable snapshot into writable storage.
static int ImmutableRamdiskWrite(Device*, uint64_t, const void*, uint64_t) {return -30;}
static int ImmutableRamdiskIoctl(Device*, uint32_t cmd, void*) {
    if(cmd==DEVICE_IOCTL_READ_ONLY)return 1;
    return cmd==DEVICE_IOCTL_FLUSH?0:-95;
}
static const BlockDeviceOps g_immutableRamdiskOps={
    RamdiskRead,ImmutableRamdiskWrite,ImmutableRamdiskIoctl,RamdiskClose,RamdiskBlockCount,RamdiskBlockSize
};

// ---- Factory ----

Device* RamdiskCreate(void* data, uint64_t size, uint32_t blockSize, const char* name)
{
    if (!data || size == 0 || blockSize == 0 || !name)
    {
        SerialPuts("RamdiskCreate: invalid parameters\n");
        return nullptr;
    }
    if (size % blockSize != 0)
    {
        SerialPrintf("RamdiskCreate: size %lu not a multiple of blockSize %u\n",
                     size, blockSize);
        return nullptr;
    }

    auto* dev  = static_cast<Device*>(kmalloc(sizeof(Device)));
    auto* priv = static_cast<RamdiskPriv*>(kmalloc(sizeof(RamdiskPriv)));

    if (!dev || !priv)
    {
        SerialPuts("RamdiskCreate: heap allocation failed\n");
        kfree(priv);kfree(dev);
        return nullptr;
    }

    priv->data      = static_cast<uint8_t*>(data);
    priv->size      = size;
    priv->blockSize = blockSize;

    dev->ops  = reinterpret_cast<const DeviceOps*>(&g_ramdiskOps);
    dev->name = name;
    dev->type = DeviceType::Block;
    dev->priv = priv;

    return dev;
}

Device* ImmutableRamdiskCreate(const void* data,uint64_t size,uint32_t blockSize,const char* name) {
    auto* device=RamdiskCreate(const_cast<void*>(data),size,blockSize,name);
    if(device)device->ops=reinterpret_cast<const DeviceOps*>(&g_immutableRamdiskOps);
    return device;
}
void RamdiskDestroyUnregistered(Device* device) {
    if(!device)return;
    // Caller must not have published this object or retained filesystem users.
    kfree(device->priv);kfree(device);
}

} // namespace brook
