#pragma once
#include <stdint.h>

namespace brook {
// Physical storage transfer accounting for the desktop R/W indicator,
// /proc/diskstats and bridge stats. Only bottom-level drivers record, once per
// completed device command (including read-modify-write sectors), so partition
// and filesystem adapters layered above them are never counted twice. Cache
// hits are not device transfers. Lock-free and panic-safe (relaxed atomics).
enum class StorageIoSource : uint32_t { Ahci=0, Usb=1, Nvme=2, Count=3 };
struct StorageIoTotals { uint64_t readOps=0, writeOps=0, flushOps=0, readBytes=0, writeBytes=0; };
struct StorageIoCounters {
    uint64_t readOps=0, writeOps=0, flushOps=0, readBytes=0, writeBytes=0;
    void Read(uint64_t bytes) {
        __atomic_fetch_add(&readOps,1,__ATOMIC_RELAXED);__atomic_fetch_add(&readBytes,bytes,__ATOMIC_RELAXED);
    }
    void Write(uint64_t bytes) {
        __atomic_fetch_add(&writeOps,1,__ATOMIC_RELAXED);__atomic_fetch_add(&writeBytes,bytes,__ATOMIC_RELAXED);
    }
    void Flush() { __atomic_fetch_add(&flushOps,1,__ATOMIC_RELAXED); }
    StorageIoTotals Snapshot() const {
        StorageIoTotals t;
        t.readOps=__atomic_load_n(&readOps,__ATOMIC_RELAXED);t.writeOps=__atomic_load_n(&writeOps,__ATOMIC_RELAXED);
        t.flushOps=__atomic_load_n(&flushOps,__ATOMIC_RELAXED);t.readBytes=__atomic_load_n(&readBytes,__ATOMIC_RELAXED);
        t.writeBytes=__atomic_load_n(&writeBytes,__ATOMIC_RELAXED);
        return t;
    }
};
inline void StorageIoAdd(StorageIoTotals& sum, const StorageIoTotals& part) {
    sum.readOps+=part.readOps;sum.writeOps+=part.writeOps;sum.flushOps+=part.flushOps;
    sum.readBytes+=part.readBytes;sum.writeBytes+=part.writeBytes;
}
// kind: 0 read, 1 write, 2 flush (bytes ignored). Exported to driver modules.
extern "C" void StorageIoAccount(uint32_t source, uint32_t kind, uint64_t bytes);
StorageIoTotals StorageIoRead(StorageIoSource source);
// Sum of every source; virtio-blk keeps its own per-device statistics.
StorageIoTotals StorageIoReadAll();
const char* StorageIoSourceName(StorageIoSource source);
} // namespace brook
