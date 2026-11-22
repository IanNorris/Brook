#include <cassert>
#include "../../drivers/vmware_svga/svga_fifo.h"
#include "../../drivers/vmware_svga/svga_cursor.h"

int main()
{
    using namespace brook::svga;
    unsigned reads = 0, pauses = 0;
    assert(WaitForFifoIdle([&] { return ++reads < 4; }, [&] { ++pauses; }));
    assert(reads == 4 && pauses == 3);
    reads = pauses = 0;
    assert(!WaitForFifoIdle([&] { ++reads; return true; }, [&] { ++pauses; }));
    assert(reads == 256 && pauses == 256);

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

    brook::CursorImage image;
    image.width = 3;
    image.height = 2;
    image.hotX = 1;
    image.hotY = 1;
    uint32_t cursor[CursorCommandWords]{};
    uint32_t count = 0;
    assert(EncodeCursor(image, false, cursor, count) == CursorPacket::Hidden);
    image.pixels[0] = 0xff123456;
    image.pixels[brook::CursorCapacity + 2] = 0xff876543;
    assert(EncodeCursor(image, false, cursor, count) == CursorPacket::Image);
    assert(count == 8 + 2 + 64);
    assert(cursor[0] == 19 && cursor[2] == 1 && cursor[3] == 1 && cursor[4] == 32);
    assert(cursor[8] == 0xffffff7f && cursor[9] == 0xffffffdf);
    assert(cursor[10] == 0x123456 && cursor[11] == 0 && cursor[44] == 0x876543);
    image.pixels[0] = 0x80ff8040;
    assert(EncodeCursor(image, false, cursor, count) == CursorPacket::Invalid);
    assert(EncodeCursor(image, true, cursor, count) == CursorPacket::Image);
    assert(count == 12 && cursor[0] == 22 && cursor[4] == 3 && cursor[6] == 0x80804020);

    auto same = image;
    same.pixels[3] = 0xff112233;
    assert(CursorImageMatches(image, same));
    same.hotX = 2;
    assert(!CursorImageMatches(image, same));
    image.hotX = -1;
    assert(EncodeCursor(image, true, cursor, count) == CursorPacket::Invalid);
}
