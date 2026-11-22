#pragma once
#include <stdint.h>

namespace brook::svga {

constexpr uint32_t FifoMin = 0;
constexpr uint32_t FifoMax = 1;
constexpr uint32_t FifoNext = 2;
constexpr uint32_t FifoStop = 3;
constexpr uint32_t Update = 1;
constexpr uint32_t MinCommandBytes = 10 * 1024;

// SYNC can kick an asynchronous host. Only sample FIFO_STOP after the
// device reports idle. The read cap also bounds waits before the clock runs.
template<class ReadBusy, class Pause>
bool WaitForFifoIdle(ReadBusy readBusy, Pause pause)
{
    for (unsigned read = 0; read < 256; ++read) {
        if (!readBusy()) return true;
        pause();
    }
    return false;
}

struct FifoWindow {
    uint32_t min;
    uint32_t max;
    uint32_t next;
    uint32_t stop;
};

inline bool ValidFifo(FifoWindow fifo, uint32_t mappedBytes)
{
    return !((fifo.min | fifo.max | fifo.next | fifo.stop) & 3) &&
           fifo.min >= 4 * sizeof(uint32_t) && fifo.min < fifo.max &&
           fifo.max <= mappedBytes && fifo.max - fifo.min >= MinCommandBytes &&
           fifo.next >= fifo.min && fifo.next < fifo.max &&
           fifo.stop >= fifo.min && fifo.stop < fifo.max;
}

inline bool FifoCanWrite(FifoWindow fifo, uint32_t mappedBytes, uint32_t words)
{
    if (!words || !ValidFifo(fifo, mappedBytes)) return false;
    const uint32_t space = fifo.next < fifo.stop ? fifo.stop - fifo.next
        : fifo.max - fifo.next + fifo.stop - fifo.min;
    // Reserve a word so an empty queue and full queue never share the same
    // NEXT/STOP values.
    return words < space / sizeof(uint32_t);
}

inline uint32_t FifoAdvance(FifoWindow fifo, uint32_t cursor)
{
    const uint32_t next = cursor + sizeof(uint32_t);
    return next == fifo.max ? fifo.min : next;
}

struct Geometry {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t offset;
};

inline bool FitsFramebuffer(Geometry mode, uint32_t maxWidth, uint32_t maxHeight,
                            uint64_t aperture, uint64_t reportedBytes)
{
    if (!mode.width || !mode.height || mode.width > maxWidth || mode.height > maxHeight ||
        (mode.offset & 4095u) || (mode.stride & 3) ||
        uint64_t(mode.width) * 4 > mode.stride || mode.offset > aperture) return false;
    const uint64_t bytes = uint64_t(mode.height) * mode.stride;
    return bytes <= aperture - mode.offset && bytes <= reportedBytes;
}

inline bool UpdateSpan(Geometry mode, uint32_t first, uint32_t end, uint32_t (&words)[5])
{
    if (end > mode.height) end = mode.height;
    if (!mode.width || first >= end) return false;
    words[0] = Update;
    words[1] = 0;
    words[2] = first;
    words[3] = mode.width;
    words[4] = end - first;
    return true;
}

}
