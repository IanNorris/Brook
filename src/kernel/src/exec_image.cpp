#include "exec_image.h"
#include "memory/exec_page_cache.h"
#include "vfs.h"
#include "memory/virtual_memory.h"
#include "process.h"
#include "string.h"

namespace brook {
void FreeExecutableImage(ExecutableImage* image)
{
    const bool released = image->data != nullptr;
    if (released)
        VmmFreePages(VirtualAddress(reinterpret_cast<uint64_t>(image->data)), image->pages);
    *image = {};
    // Covers failed construction whose AS teardown preceded staging release.
    if (released) ExecPageCacheTrimIdle();
}

int ReadExecutableImage(Vnode* vnode, ExecutableImage* image)
{
    *image = {};
    VnodeStat st{};
    bool known = VfsStat(vnode, &st) == 0 && st.size != 0;
    if (known && st.size > MAX_EXEC_IMAGE_BYTES) return -27; // EFBIG
    uint64_t capacity = known ? st.size : 65536;
    auto allocate = [&](uint64_t bytes) {
        uint64_t pages = (bytes + 4095) / 4096;
        auto address = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Heap, KernelPid);
        if (!address) return false;
        auto* data = reinterpret_cast<uint8_t*>(address.raw());
        if (image->size) memcpy(data, image->data, image->size);
        if (image->data)
            VmmFreePages(VirtualAddress(reinterpret_cast<uint64_t>(image->data)), image->pages);
        image->data = data;
        image->pages = pages;
        return true;
    };
    if (!allocate(capacity)) return -12; // ENOMEM
    uint64_t offset = 0;
    for (;;) {
        if (image->size == capacity) {
            if (known) return 0;
            // A full unknown-size stream needs an EOF check before growing.
            uint8_t next;
            int result = VfsRead(vnode, &next, 1, &offset);
            if (!result) return 0;
            if (result < 0) { FreeExecutableImage(image); return -5; }
            if (capacity == MAX_EXEC_IMAGE_BYTES) { FreeExecutableImage(image); return -27; }
            uint64_t grown = capacity * 2;
            if (grown > MAX_EXEC_IMAGE_BYTES) grown = MAX_EXEC_IMAGE_BYTES;
            if (!allocate(grown)) { FreeExecutableImage(image); return -12; }
            capacity = grown;
            image->data[image->size++] = next;
        }
        uint64_t count = capacity - image->size;
        if (count > 65536) count = 65536;
        int result = VfsRead(vnode, image->data + image->size, count, &offset);
        if (result < 0 || static_cast<uint64_t>(result) > count ||
            (result == 0 && known && image->size != st.size)) {
            FreeExecutableImage(image);
            return -5; // EIO: never submit a short image as the full executable.
        }
        if (!result) return 0;
        image->size += static_cast<uint64_t>(result);
    }
}
}
