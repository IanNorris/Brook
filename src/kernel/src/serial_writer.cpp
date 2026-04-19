// serial_writer.cpp — Async serial/TTY output via lockless MPSC queue.
//
// All userspace writes (sys_write fd 1/2) and profiler drain output enqueue
// into g_serialQueue.  A dedicated kernel thread (serial_wr) is the sole
// consumer: it is the only code that ever calls SerialPutChar after the
// scheduler starts, so the serial lock is no longer needed in the writer
// thread's hot path.
//
// The queue is a lockless MPSC slot ring (see mpscqueue.h).  Producers claim
// slots atomically and return immediately after memcpy; no lock, no busy-wait
// on the UART.  Only serial_wr busy-waits on COM1_LSR, and it holds the
// serial lock for ≤ one slot (≤ 512 bytes ≈ 44 ms) at a time, allowing
// direct SerialPrintf callers (kernel init, KernelPanic) to still interleave.

#include "serial_writer.h"
#include "mpscqueue.h"
#include "serial.h"
#include "tty.h"
#include "process.h"
#include "scheduler.h"

// Defined in apic.cpp — monotonic tick counter (1 tick ≈ 1ms).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

namespace brook {

// 2048 slots × 512 bytes = ~1 MB of queue memory.
// With 2048 slots, 8 producers and a ~11 KB/s UART backend, the queue absorbs
// ~90 seconds of flat-out burst before producers start spinning (in practice
// much longer since most messages are short).
static MpscQueue<2048, 512> g_serialQueue;

// The writer thread process (kept for unblocking on wakeup).
static Process* g_writerProc = nullptr;

// ---------------------------------------------------------------------------
// Writer kernel thread — single consumer, sole UART writer post-scheduler.
// ---------------------------------------------------------------------------

static void SerialWriterThreadFn(void* /*arg*/)
{
    char slot[512];

    for (;;) {
        uint32_t n = g_serialQueue.dequeue(slot, sizeof(slot));

        if (n > 0) {
            // serial_wr is the only post-boot UART writer, so the lock hold
            // is just for safety against early-boot SerialPrintf calls and
            // KernelPanic.  One slot (≤ 512 B) takes at most ~44 ms at
            // 115200 baud — acceptable since those callers are rare.
            SerialLock();
            for (uint32_t i = 0; i < n; ++i)
                SerialPutChar(slot[i]);
            SerialUnlock();

            // Echo to TTY (framebuffer console) if available.
            if (TtyReady()) {
                for (uint32_t i = 0; i < n; ++i)
                    TtyPutChar(slot[i]);
            }
        } else {
            // Queue empty — sleep briefly.
            Process* self = ProcessCurrent();
            if (self) {
                self->wakeupTick = g_lapicTickCount + 5;
                SchedulerBlock(self);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SerialWriterInit()
{
    g_writerProc = KernelThreadCreate("serial_wr", SerialWriterThreadFn, nullptr,
                                       2 /* NORMAL priority */);
    if (g_writerProc) {
        SchedulerAddProcess(g_writerProc);
        SerialPrintf("SERIAL_WRITER: thread created pid=%u\n", g_writerProc->pid);
    }
}

void SerialWriterEnqueue(const char* buf, uint32_t len)
{
    // Split writes that exceed one slot so no message is silently truncated.
    constexpr uint32_t kSlotBytes = 512;
    while (len > 0) {
        uint32_t chunk = (len > kSlotBytes) ? kSlotBytes : len;
        g_serialQueue.enqueue(buf, chunk);
        buf += chunk;
        len -= chunk;
    }
}

} // namespace brook

