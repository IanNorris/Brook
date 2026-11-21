#include <cassert>
#include "../../drivers/vmware_svga/svga_fifo.h"

int main()
{
    using namespace brook::svga;
    constexpr uint32_t Bytes = 0x10000;
    constexpr uint32_t Start = 0x1000;
    assert(FifoCanWrite({Start, Bytes, Start, Start}, Bytes, 5));
    assert(FifoCanWrite({Start, Bytes, Bytes - 8, Start + 16}, Bytes, 5));
    assert(!FifoCanWrite({Start, Bytes, Bytes - 8, Start + 12}, Bytes, 5));
    assert(!FifoCanWrite({Start, Bytes, Start + 4, Start + 8}, Bytes, 1));
    assert(!FifoCanWrite({Start, Bytes + 4, Start + 4, Start + 8}, Bytes, 1));
    assert(!FifoCanWrite({Bytes - 8, Bytes, Bytes - 4, Bytes - 8}, Bytes, 1));
    assert(!FifoCanWrite({Start + 1, Bytes, Start + 4, Start + 8}, Bytes, 1));
    assert(FifoAdvance({Start, Bytes, Bytes - 4, Start + 12}, Bytes - 4) == Start);

    assert(FitsFramebuffer({1280, 720, 5120, 4096}, 1920, 1080, 8u << 20, 8u << 20));
    assert(!FitsFramebuffer({1280, 720, 5116, 0}, 1920, 1080, 8u << 20, 8u << 20));
    assert(!FitsFramebuffer({1280, 720, 5120, 4095}, 1920, 1080, 8u << 20, 8u << 20));
    assert(!FitsFramebuffer({1280, 720, 5120, 4096}, 1920, 1080, 5120u * 720u, 8u << 20));
    assert(!FitsFramebuffer({1280, 720, 5120, 0}, 1024, 1080, 8u << 20, 8u << 20));

    uint32_t update[5]{};
    assert(UpdateSpan({1280, 720, 5120, 0}, 700, 800, update));
    assert(update[0] == Update && update[1] == 0 && update[2] == 700 &&
           update[3] == 1280 && update[4] == 20);
    assert(!UpdateSpan({1280, 720, 5120, 0}, 800, 840, update));
}
