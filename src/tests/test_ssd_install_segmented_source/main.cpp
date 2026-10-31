#include "../../kernel/src/ssd_install_segmented_source.h"
#include <cassert>
#include <cstdint>
#include <vector>
using namespace brook;
struct Reader {
    int calls = 0, failAt = -1;
    bool shortRead = false;
    int readSegment(uint32_t index, uint64_t offset, void* dst, uint32_t n) {
        if (calls++ == failAt) return -1;
        if (shortRead) return int(n - 1);
        auto* p = static_cast<uint8_t*>(dst);
        for (uint32_t i = 0; i < n; ++i) p[i] = uint8_t((index * 17 + offset + i) & 255);
        return int(n);
    }
};
int main() {
    constexpr uint64_t chunk = 2ull * 1024 * 1024 * 1024;
    SsdInstallSegmentedSource source(5 * chunk + 17, chunk, 6);
    assert(source.Valid() && source.ExpectedSize(0) == chunk && source.ExpectedSize(5) == 17);
    assert(source.ExpectedSize(6) == 0);
    Reader r; uint8_t out[32]{};
    assert(source.Read(r, chunk - 8, out, 32) == 32 && r.calls == 2);
    for (int i = 0; i < 8; ++i) assert(out[i] == uint8_t(chunk - 8 + i));
    for (int i = 0; i < 24; ++i) assert(out[8 + i] == uint8_t(17 + i));
    assert(source.Read(r, 5 * chunk + 16, out, 1) == 1 && out[0] == uint8_t(5 * 17 + 16));
    assert(source.Read(r, 5 * chunk + 17, nullptr, 0) == 0);
    assert(source.Read(r, 5 * chunk + 17, out, 1) < 0);
    assert(source.Read(r, UINT64_MAX, out, 1) < 0);
    assert(source.Read(r, 0, nullptr, 1) < 0);
    Reader fail; fail.failAt = 1; assert(source.Read(fail, chunk - 8, out, 32) < 0 && fail.calls == 2);
    Reader shortReader; shortReader.shortRead = true;
    assert(source.Read(shortReader, 0, out, 32) < 0 && shortReader.calls == 1);
    Reader oversized; assert(source.Read(oversized, 0, out, uint32_t(INT_MAX) + 1) < 0 && oversized.calls == 0);
    SsdInstallSegmentedSource exact(2 * chunk, chunk, 2);
    assert(exact.Valid() && exact.ExpectedSize(1) == chunk);
    SsdInstallSegmentedSource maximum(UINT64_MAX, UINT64_MAX, 1);
    assert(maximum.Valid() && maximum.ExpectedSize(0) == UINT64_MAX);
    assert(!SsdInstallSegmentedSource(0, chunk, 1).Valid());
    assert(!SsdInstallSegmentedSource(chunk + 1, chunk, 1).Valid());
    assert(!SsdInstallSegmentedSource(chunk, 0, 1).Valid());
    assert(!SsdInstallSegmentedSource(chunk, chunk, 0).Valid());
}
