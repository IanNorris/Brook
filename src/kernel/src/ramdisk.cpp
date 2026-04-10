#include "ramdisk.h"
#include "heap.h"
#include "serial.h"

namespace brook {

// ---- Private state ----

struct RamdiskPriv {
    uint8_t*  data;
    uint64_t  size;       // total bytes
    uint32_t  blockSize;
};

// ---- DeviceOps implementation ----

static int RamdiskRead(Device* dev, uint64_t offset, void* buf, uint64_t len)
{
    SerialPuts("RamdiskRead: entered\n");
    auto* p = static_cast<RamdiskPriv*>(dev->priv);
    SerialPrintf("RamdiskRead: p=%p size=%lu\n", (void*)p, p ? p->size : 0UL);
    if (offset >= p->size) return 0;
    if (offset + len > p->size) len = p->size - offset;

    const uint8_t* src = p->data + offset;
    uint8_t*       dst = static_cast<uint8_t*>(buf);
    for (uint64_t i = 0; i < len; ++i) dst[i] = src[i];
    SerialPuts("RamdiskRead: done\n");
    return static_cast<int>(len);
}

static int RamdiskWrite(Device* dev, uint64_t offset, const void* buf, uint64_t len)
{
    auto* p = static_cast<RamdiskPriv*>(dev->priv);
    if (offset >= p->size) return 0;
    if (offset + len > p->size) len = p->size - offset;

    const uint8_t* src = static_cast<const uint8_t*>(buf);
    uint8_t*       dst = p->data + offset;
    for (uint64_t i = 0; i < len; ++i) dst[i] = src[i];
    return static_cast<int>(len);
}

static int RamdiskIoctl(Device* /*dev*/, uint32_t /*cmd*/, void* /*arg*/)
{
    return -1; // not implemented
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

} // namespace brook
