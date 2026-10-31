// Host wrapper around the same kernel eligibility check used by EFI.
#include "../src/shared/update/executable.h"
#include <cstdio>
#include <vector>

int main() {
    std::vector<uint8_t> data;
    uint8_t buffer[65536];
    while (true) {
        const size_t n = std::fread(buffer, 1, sizeof(buffer), stdin);
        if (data.size() + n > BROOK_UPDATE_KERNEL_MAX) return 2;
        data.insert(data.end(), buffer, buffer + n);
        if (n < sizeof(buffer)) {
            if (std::ferror(stdin)) return 2;
            break;
        }
    }
    return BrookUpdateExecutable(data.data(), data.size()) ? 0 : 1;
}
