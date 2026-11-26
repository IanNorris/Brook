#include <cassert>
#include <cerrno>
#include <cstring>

#include "devtmpfs.h"
#include "fd_table.h"
#include "fido_hidraw.h"
#include "process.h"
#include "serial.h"
#include "sync/kmutex.h"
#include "tmpfs.h"
#include "user_copy.h"
#include "vfs.h"

namespace brook {

static FdEntry opened;
static OpenFile description;
static uint64_t published;
static uint32_t submitted;
static unsigned sleeps;
static uint32_t feedingIdentity;

int FdAlloc(Process*, FdType type, void* handle, uint32_t flags, uint8_t, uint8_t)
{
    description.statusFlags = flags;
    opened.type = type;
    opened.handle = handle;
    opened.file = &description;
    return 0;
}

int TmpfsSeedEntry(Tmpfs*, const char* name, uint16_t mode, uint64_t device, const char*,
                   uint32_t uid, uint32_t gid)
{
    assert(!std::strcmp(name, "hidraw0") && mode == 0020600 && uid == 1000 && gid == 1000);
    assert(!published);
    published = device;
    return 0;
}
void TmpfsRemoveCharDevice(Tmpfs*, const char* name, uint64_t device)
{
    assert(!std::strcmp(name, "hidraw0") && device == published);
    published = 0;
}

extern "C" void SerialPrintf(const char*, ...) {}
bool ModuleRetainAddresses(const uintptr_t*, uint32_t) { return true; }
void KMutexLock(KMutex*) {}
void KMutexUnlock(KMutex*) {}
bool HasPendingSignals() { return false; }
bool UserBufferWritable(uint64_t address, uint64_t) { return address != 0; }
uint64_t CopyToUserBytes(uint64_t out, const void* in, uint64_t n)
{
    if (!out) return 0;
    std::memcpy(reinterpret_cast<void*>(out), in, n);
    return n;
}
uint64_t CopyFromUserBytes(void* out, uint64_t in, uint64_t n)
{
    if (!in) return 0;
    std::memcpy(out, reinterpret_cast<const void*>(in), n);
    return n;
}
extern "C" void SchedulerSleepMs(uint32_t ms)
{
    assert(ms == 5 && feedingIdentity);
    ++sleeps;
    uint8_t report[HidrawReportBytes] = {0x51};
    HidrawReceive(feedingIdentity, report);
    feedingIdentity = 0;
}

int queue(void*, uint32_t identity, const uint8_t* report)
{
    assert(report[0] == 0x23);
    submitted = identity;
    return 0;
}
} // namespace brook

int main()
{
    using namespace brook;
    constexpr uint8_t desc[] = {0x06, 0xd0, 0xf1, 0x09, 0x01};
    constexpr uint64_t getSize = 0x80044801, getDesc = 0x90044802, getInfo = 0x80084803;
    HidrawMount(reinterpret_cast<Tmpfs*>(1));
    const auto identity = HidrawAttach(0x1050, 0x0406, desc, sizeof(desc), queue, nullptr);
    assert(identity && !published);
    HidrawActivate(identity);
    auto makeStat = [](uint64_t dev) {
        VnodeStat metadata{};
        metadata.rdev = dev;
        metadata.uid = metadata.gid = 1000;
        metadata.mode = 0020600;
        metadata.ino = 9;
        return metadata;
    };
    const auto oldDevice = published;
    assert(oldDevice && HidrawOpen(nullptr, makeStat(oldDevice), 0x802) == 0);
    auto oldEntry = opened;
    auto* old = static_cast<HidrawFile*>(opened.handle);

    uint32_t size = 0;
    assert(HidrawIoctl(old, getSize, reinterpret_cast<uint64_t>(&size)) == 0 && size == sizeof(desc));
    struct { uint32_t size; uint8_t bytes[16]; } buffer{3, {}};
    assert(HidrawIoctl(old, getDesc, reinterpret_cast<uint64_t>(&buffer)) == 0);
    assert(!std::memcmp(buffer.bytes, desc, 3) && buffer.bytes[3] == 0);
    buffer.size = 4097;
    assert(HidrawIoctl(old, getDesc, reinterpret_cast<uint64_t>(&buffer)) == -EINVAL);
    assert(HidrawIoctl(old, getDesc, 0) == -EFAULT);
    assert(HidrawIoctl(old, 1, 0) == -ENOTTY);
    struct { uint32_t bus; uint16_t vid, pid; } info{};
    assert(HidrawIoctl(old, getInfo, reinterpret_cast<uint64_t>(&info)) == 0);
    assert(info.bus == 3 && info.vid == 0x1050 && info.pid == 0x0406);

    uint8_t report[65] = {0, 0x23};
    const auto address = reinterpret_cast<uint64_t>(report);
    assert(HidrawReady(old, 5) == 4);
    assert(HidrawRead(&opened, address, 64) == -EAGAIN);
    assert(HidrawWrite(&opened, address, 64, true) == -EINVAL);
    assert(HidrawWrite(&opened, address, 65, true) == 65 && submitted == identity);
    assert(HidrawReady(old, 5) == 0 && HidrawWrite(&opened, address, 65, true) == -EAGAIN);
    HidrawOutputComplete(identity, true);

    HidrawReceive(identity, report);
    HidrawReceive(identity, report);
    assert(HidrawReady(old, 5) == 5);
    assert(HidrawRead(&opened, address, 17) == 17);
    assert(HidrawRead(&opened, address, 65) == 64);
    assert(HidrawRead(&opened, address, 64) == -EAGAIN);
    description.statusFlags = 2;
    feedingIdentity = identity;
    assert(HidrawRead(&opened, address, 64) == 64 && report[0] == 0x51 && sleeps == 1);

    HidrawDetach(identity);
    assert(!published && HidrawReady(old, 1) == 24);
    assert(HidrawRead(&oldEntry, address, 64) == -ENODEV);
    assert(HidrawIoctl(old, getSize, reinterpret_cast<uint64_t>(&size)) == -ENODEV);
    const auto second = HidrawAttach(0x46f4, 5, desc, sizeof(desc), queue, nullptr);
    HidrawActivate(second);
    assert(second != identity && published != oldDevice);
    assert(HidrawOpen(nullptr, makeStat(oldDevice), 2) == -ENODEV);
    assert(HidrawOpen(nullptr, makeStat(published), 0x802) == 0);
    HidrawReceive(identity, report);
    assert(HidrawRead(&opened, address, 64) == -EAGAIN);
    HidrawReceive(second, report);
    assert(HidrawRead(&opened, address, 64) == 64);
    HidrawUnref(old);
    HidrawUnref(static_cast<HidrawFile*>(opened.handle));
    HidrawDetach(second);
}
