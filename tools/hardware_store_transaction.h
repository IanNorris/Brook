#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../src/shared/update/payload.h"
/* Caller authenticates and retains the directory FD and shared hardware-update
 * lock through destruction. Never removes an incomplete image or overwrites a
 * previously published file. Publication may be uncertain after an error. */
class HardwareStoreTransaction {
public:
    HardwareStoreTransaction() = default;
    ~HardwareStoreTransaction();
    HardwareStoreTransaction(const HardwareStoreTransaction&) = delete;
    HardwareStoreTransaction& operator=(const HardwareStoreTransaction&) = delete;
    int begin(int directory, const char *record, size_t length, const uint8_t digest[32]);
    int append(const void *data, size_t length);
    int stage();
    int finalize();
    bool marker_published() const { return published_; }
    bool marker_durable() const { return durable_; }
    bool image_created() const { return created_; }
private:
    int directory_ = -1, file_ = -1;
    uint64_t size_ = 0, written_ = 0;
    uint64_t device_ = 0, inode_ = 0;
    char record_[97]{};
    size_t record_length_ = 0;
    uint8_t digest_[32]{};
    BrookSha256 stream_{};
    bool begun_ = false, staged_ = false, attempted_ = false, failed_ = false;
    bool published_ = false, durable_ = false, created_ = false;
};
