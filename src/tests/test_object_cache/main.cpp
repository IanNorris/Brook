#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <atomic>
#include <thread>
#include <vector>
#include "../../kernel/src/memory/object_cache.h"

struct alignas(64) Object { unsigned value; };

int main() {
    brook::ObjectCache<Object, 3> cache{};
    Object objects[4]{};
    assert(!cache.TryAcquire());
    assert(!cache.Release(nullptr));
    for (unsigned i = 0; i < 3; ++i) {
        objects[i].value = i;
        assert(cache.Release(&objects[i]));
    }
    assert(!cache.Release(&objects[3]));
    bool seen[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        Object* object = cache.TryAcquire();
        assert(object && object->value < 3 && !seen[object->value]);
        seen[object->value] = true;
    }
    assert(!cache.TryAcquire());
    // All original objects held by sleepers: any completed fallback can refill.
    assert(cache.Release(&objects[3]));
    assert(cache.TryAcquire() == &objects[3]);
    auto stats = cache.Snapshot();
    assert(stats.hits == 4 && stats.misses == 2 && stats.retained == 4 && stats.full == 1);

    for (unsigned i = 0; i < 3; ++i) assert(cache.Release(&objects[i]));
    std::atomic<unsigned> active[3]{};
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < 8; ++worker) threads.emplace_back([&, worker] {
        for (unsigned iteration = 0; iteration < 5000; ++iteration) {
            Object* object;
            while (!(object = cache.TryAcquire())) std::this_thread::yield();
            const unsigned index = object - objects;
            assert(index < 3);
            assert(active[index].fetch_add(1) == 0);
            object->value = worker;
            assert(object->value == worker);
            assert(active[index].fetch_sub(1) == 1);
            assert(cache.Release(object));
        }
    });
    for (auto& thread : threads) thread.join();
    bool returned[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        Object* object = cache.TryAcquire();
        assert(object && !returned[object - objects]);
        returned[object - objects] = true;
    }
    assert(!cache.TryAcquire());
}
