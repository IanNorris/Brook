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

// Set to 1 when writer is about to block (or is blocked) on an empty queue.
// Producers check this and call SchedulerUnblock to wake the consumer.
static volatile uint32_t g_writerSleeping = 0;

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
            // Queue empty — block indefinitely until SerialWriterEnqueue wakes us.
            // The previous "wakeupTick = lapicTick + 5" pattern caused a high-rate
            // self-poll cycle that exercised SchedulerBlock/Unblock every 5ms even
            // when nothing needed printing; this was implicated in an SMP scheduler
            // wakeup race where userspace processes failed to dispatch.
            Process* self = ProcessCurrent();
            if (self) {
                __atomic_store_n(&g_writerSleeping, 1, __ATOMIC_RELEASE);
                // Re-check queue under the sleeping flag to close the wake race.
                // If a producer enqueued between our dequeue and setting the flag,
                // it will have set g_writerSleeping=0 + called SchedulerUnblock
                // (which sets pendingWakeup if we're not yet Blocked).
                if (g_serialQueue.empty()) {
                    self->wakeupTick = 0;
                    SchedulerBlock(self);
                }
                __atomic_store_n(&g_writerSleeping, 0, __ATOMIC_RELEASE);
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

    // Wake the consumer if it's sleeping.  The CAS ensures we only call
    // SchedulerUnblock once per sleep cycle (cheap when not sleeping).
    if (__atomic_load_n(&g_writerSleeping, __ATOMIC_ACQUIRE) != 0) {
        uint32_t expected = 1;
        if (__atomic_compare_exchange_n(&g_writerSleeping, &expected, 0,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            if (g_writerProc) {
                SchedulerUnblock(g_writerProc);
            }
        }
    }
}

} // namespace brook

