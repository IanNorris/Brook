#pragma once

#include "memory/heap.h"
#include "path_limits.h"

namespace brook {

// Path-sized scratch stays off the kernel stack. Allocation is lazy so
// ordinary retained lookups need no buffer until they follow a symlink.
class PathBuffer {
public:
    explicit PathBuffer(uint32_t capacity = PathMax) : capacity_(capacity) {}
    ~PathBuffer() { if (data_) kfree(data_); }
    PathBuffer(const PathBuffer&) = delete;
    PathBuffer& operator=(const PathBuffer&) = delete;

    bool allocate() {
        if (!data_) data_ = static_cast<char*>(kmalloc(capacity_));
        if (!data_) return false;
        data_[0] = 0;
        return true;
    }
    uint32_t capacity() const { return capacity_; }
    char* data() const { return data_; }
    operator char*() const { return data_; }

private:
    char* data_ = nullptr;
    uint32_t capacity_;
};

inline uint32_t PathLength(const char* text, uint32_t limit = PathMax)
{
    uint32_t length = 0;
    while (length < limit && text[length]) ++length;
    return length;
}

}
