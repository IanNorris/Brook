// serial_writer.cpp — Async serial/TTY output via kernel thread + ring buffer.
//
// Userspace sys_write(fd 1/2) enqueues into g_serialBuf.  A dedicated kernel
// thread drains the buffer to the serial port (and optionally TTY), decoupling
// userspace from the 115200 baud bottleneck.

#include "serial_writer.h"
#include "kringbuf.h"
#include "serial.h"
#include "tty.h"
#include "process.h"
#include "scheduler.h"

// Defined in apic.cpp — monotonic tick counter (1 tick ≈ 1ms).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

namespace brook {

// 64 KB ring buffer — enough for bursty printf output from 32 processes.
static KRingBuffer<65536> g_serialBuf;

// The writer thread process (so we can unblock it).
static Process* g_writerProc = nullptr;

// ---------------------------------------------------------------------------
// Writer kernel thread
// ---------------------------------------------------------------------------

static void SerialWriterThreadFn(void* /*arg*/)
{
    char batch[256];  // Drain in 256-byte chunks

    for (;;) {
        // Drain everything currently in the buffer.
        for (;;) {
            uint32_t n = g_serialBuf.read(batch, sizeof(batch));
            if (n == 0) break;

            SerialLock();
            for (uint32_t i = 0; i < n; ++i)
                SerialPutChar(batch[i]);
            SerialUnlock();

            // Also echo to TTY (framebuffer console) if available.
            if (TtyReady()) {
                for (uint32_t i = 0; i < n; ++i)
                    TtyPutChar(batch[i]);
            }
        }

        // Sleep 5ms then check again.  Short sleep keeps latency low
        // while avoiding busy-spin.
        Process* self = ProcessCurrent();
        if (self) {
            self->wakeupTick = g_lapicTickCount + 5;
            SchedulerBlock(self);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SerialWriterInit()
{
    g_writerProc = KernelThreadCreate("serial_wr", SerialWriterThreadFn, nullptr,
                                       1 /* HIGH priority — below RT, above normal */);
    if (g_writerProc)
        SerialPrintf("SERIAL_WRITER: thread created pid=%u\n", g_writerProc->pid);
}

void SerialWriterEnqueue(const char* buf, uint32_t len)
{
    g_serialBuf.write(buf, len);
}

} // namespace brook
