#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../src/shared/update/payload.h"
/* Caller authenticates and retains the directory FD and shared hardware-update
 * lock through destruction. Never overwrites a published file. A v3 request
 * stages in a separate temporary file until authenticated finalization. */
class HardwareStoreTransaction {
public:
    explicit HardwareStoreTransaction(bool temporary = false) : temporary_(temporary) {}
    ~HardwareStoreTransaction();
    HardwareStoreTransaction(const HardwareStoreTransaction&) = delete;
    HardwareStoreTransaction& operator=(const HardwareStoreTransaction&) = delete;
    int begin(int directory, const char *record, size_t length, const uint8_t digest[32]);
    int append(const void *data, size_t length);
    int stage();
    int finalize();
    int discard_staging();
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
    const bool temporary_;
    bool image_published_ = false;
    const char* image_name() const { return temporary_ ? "STORE.TMP" : "STORE.IMG"; }
};
