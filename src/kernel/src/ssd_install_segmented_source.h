#pragma once
#include <stdint.h>
#include <limits.h>

namespace brook {
// Maps a logical image to fixed-sized, already pinned source segments. The
// caller must validate each actual file size and hold all required leases for
// the lifetime of the installer. This is mapping only, not authorization or
// authentication of the image.
class SsdInstallSegmentedSource {
    uint64_t total_, segment_;
    uint32_t count_;
public:
    SsdInstallSegmentedSource(uint64_t total, uint64_t segment, uint32_t count)
        : total_(total), segment_(segment), count_(count) {}
    bool Valid() const {
        return total_ && segment_ && count_ && (total_ - 1) / segment_ == uint64_t(count_ - 1);
    }
    uint64_t ExpectedSize(uint32_t index) const {
        if (!Valid() || index >= count_) return 0;
        return index + 1 == count_ ? (total_ - 1) % segment_ + 1 : segment_;
    }
    template<class Reader> int Read(Reader& reader, uint64_t offset, void* dest, uint32_t bytes) const {
        if (!Valid() || bytes > INT_MAX || offset > total_ || bytes > total_ - offset || (!dest && bytes)) return -1;
        auto* output = static_cast<uint8_t*>(dest);
        uint32_t done = 0;
        while (done < bytes) {
            const uint64_t position = offset + done;
            const auto index = uint32_t(position / segment_);
            const uint64_t within = position % segment_;
            const uint64_t available = ExpectedSize(index) - within;
            const uint32_t n = available < bytes - done ? uint32_t(available) : bytes - done;
            if (reader.readSegment(index, within, output + done, n) != int(n)) return -1;
            done += n;
        }
        return int(done);
    }
};
}
