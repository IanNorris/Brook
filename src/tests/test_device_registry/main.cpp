#include "test_framework.h"
#include "device.h"

// Dummy ops for test devices
static int DummyRead(brook::Device*, uint64_t, void*, uint64_t) { return 0; }
static int DummyWrite(brook::Device*, uint64_t, const void*, uint64_t) { return 0; }
static int DummyIoctl(brook::Device*, uint32_t, void*) { return 0; }
static void DummyClose(brook::Device*) {}

static const brook::DeviceOps g_dummyOps = {
    DummyRead, DummyWrite, DummyIoctl, DummyClose
};

// Block device ops
static uint64_t FakeBlockCount(brook::Device*) { return 1024; }
static uint32_t FakeBlockSize(brook::Device*)  { return 512; }

static const brook::BlockDeviceOps g_blockOps = {
    DummyRead, DummyWrite, DummyIoctl, DummyClose, FakeBlockCount, FakeBlockSize
};

static brook::Device MakeDev(const brook::DeviceOps* ops, const char* name,
                              brook::DeviceType type)
{
    brook::Device d;
    d.ops  = ops;
    d.name = name;
    d.type = type;
    d.priv = nullptr;
    return d;
}

struct CountCtx { uint32_t count; };
static bool Counter(brook::Device*, void* ctx) {
    static_cast<CountCtx*>(ctx)->count++;
    return true;
}
static bool StopAfterOne(brook::Device*, void* ctx) {
    static_cast<CountCtx*>(ctx)->count++;
    return false;
}

TEST_MAIN("device_registry", {

    // 1. Register a char device
    brook::Device charDev = MakeDev(&g_dummyOps, "serial0", brook::DeviceType::Char);
    ASSERT_TRUE(brook::DeviceRegister(&charDev));

    // 2. Find it by name
    brook::Device* found = brook::DeviceFind("serial0");
    ASSERT_TRUE(found != nullptr);
    ASSERT_TRUE(found == &charDev);
    ASSERT_TRUE(found->type == brook::DeviceType::Char);

    // 3. Not found for non-existent name
    ASSERT_TRUE(brook::DeviceFind("nonexistent") == nullptr);
    ASSERT_TRUE(brook::DeviceFind(nullptr) == nullptr);

    // 4. Duplicate name rejected
    brook::Device dup = MakeDev(&g_dummyOps, "serial0", brook::DeviceType::Char);
    ASSERT_FALSE(brook::DeviceRegister(&dup));

    // 5. Null device/name/ops rejected
    ASSERT_FALSE(brook::DeviceRegister(nullptr));
    brook::Device noName = MakeDev(&g_dummyOps, nullptr, brook::DeviceType::Char);
    ASSERT_FALSE(brook::DeviceRegister(&noName));
    brook::Device noOps = MakeDev(nullptr, "test", brook::DeviceType::Char);
    ASSERT_FALSE(brook::DeviceRegister(&noOps));

    // 6. Register a block device
    auto* blkOpsAsGeneric = reinterpret_cast<const brook::DeviceOps*>(&g_blockOps);
    brook::Device blkDev = MakeDev(blkOpsAsGeneric, "blk0", brook::DeviceType::Block);
    ASSERT_TRUE(brook::DeviceRegister(&blkDev));

    // 7. Block device helpers
    brook::Device* blk = brook::DeviceFind("blk0");
    ASSERT_TRUE(blk != nullptr);
    ASSERT_EQ(brook::DeviceBlockCount(blk), static_cast<uint64_t>(1024));
    ASSERT_EQ(brook::DeviceBlockSize(blk), static_cast<uint32_t>(512));

    // 8. DeviceIterate — count block and char devices
    CountCtx blockCount = {0};
    brook::DeviceIterate(brook::DeviceType::Block, Counter, &blockCount);
    ASSERT_EQ(blockCount.count, static_cast<uint32_t>(1));

    CountCtx charCount = {0};
    brook::DeviceIterate(brook::DeviceType::Char, Counter, &charCount);
    ASSERT_EQ(charCount.count, static_cast<uint32_t>(1));

    CountCtx ttyCount = {0};
    brook::DeviceIterate(brook::DeviceType::Tty, Counter, &ttyCount);
    ASSERT_EQ(ttyCount.count, static_cast<uint32_t>(0));

    // 9. DeviceIterate — early stop
    brook::Device charDev2 = MakeDev(&g_dummyOps, "serial1", brook::DeviceType::Char);
    ASSERT_TRUE(brook::DeviceRegister(&charDev2));

    CountCtx earlyStop = {0};
    brook::DeviceIterate(brook::DeviceType::Char, StopAfterOne, &earlyStop);
    ASSERT_EQ(earlyStop.count, static_cast<uint32_t>(1));

    // 10. Null callback doesn't crash
    brook::DeviceIterate(brook::DeviceType::Char, nullptr, nullptr);

    brook::SerialPrintf("All device registry tests passed\n");
})

