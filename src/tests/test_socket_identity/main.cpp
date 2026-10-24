#include "../../kernel/src/socket_identity.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>

int main() {
    // Kernel globals must not require a startup constructor to acquire their
    // initial counter value. This declaration must remain a constant expression.
    constexpr brook::SocketInodeAllocator constantInitialized;
    (void)constantInitialized;
    brook::SocketInodeAllocator allocator;
    constexpr unsigned threads = 8, iterations = 10000;
    std::vector<uint64_t> results(threads * iterations);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back([&, t] {
            for (unsigned i = 0; i < iterations; ++i) results[t * iterations + i] = allocator.allocate();
        });
    for (auto& worker : workers) worker.join();
    std::sort(results.begin(), results.end());
    for (unsigned i = 0; i < results.size(); ++i) assert(results[i] == (uint64_t{1} << 62) + i);
    brook::SocketInodeAllocator end((uint64_t{1} << 63) - 1);
    assert(end.allocate() == (uint64_t{1} << 63) - 1);
    assert(end.allocate() == 0 && end.allocate() == 0);
    brook::SocketInodeAllocator invalid(0);
    assert(invalid.allocate() == 0);
    char label[64];
    assert(brook::SocketIdentityLabel(results.front(), label, sizeof(label)));
    assert(!std::strcmp(label, "socket:[4611686018427387904]"));
    char exact[11];
    assert(brook::SocketIdentityLabel(1, exact, sizeof(exact)));
    assert(!std::strcmp(exact, "socket:[1]"));
    char small[10];
    assert(!brook::SocketIdentityLabel(1, small, sizeof(small)) && small[0] == 0);
    assert(!brook::SocketIdentityLabel(1, nullptr, 0));
}
