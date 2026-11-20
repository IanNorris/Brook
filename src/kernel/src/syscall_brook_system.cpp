// syscall_brook_system.cpp -- Brook system syscalls: profiler, crash entry, input, system messages, paging, audio, AHCI export.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "system_message.h"
#include "user_access_scope.h"
#include "memory/pageable_memory.h"
#include "memory/physical_memory.h"
#include "compositor.h"
#include "ahci_readonly.h"
#include "mount_namespace.h"
#include "audio.h"
#include "profiler.h"
#include "string.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_handlers.h"

namespace brook {

// Read-only per-user retained system events. A failed user copy never consumes
// an event; the caller advances its own cursor only after receiving the record.
int64_t sys_brook_system_message(uint64_t after,uint64_t output,uint64_t size,
                                 uint64_t a,uint64_t b,uint64_t c) {
    auto* actor=ProcessCurrent();if(!actor)return -ESRCH;
    KernelOperationGuard guard(actor);
    if(a||b||c||size!=sizeof(BrookSystemMessage))return -EINVAL;
    // A prestarted presenter retains the event until there is room to render
    // it. Low-headroom diagnostics remain on the kernel debug/serial path.
    if(PmmGetFreePageCount()<8192)return 0;
    UserAccessScope lease(actor);
    if(!UserBufferWritable(output,size))return -EFAULT;
    BrookSystemMessage event{};
    if(!brook::SystemMessageReadAfter(after,ProcessReadIds(actor,false).effective,&event))return 0;
    return CopyToUser(output,&event,sizeof(event))?1:-EFAULT;
}

// Experimental explicit single-task paging; unavailable without a recognized
// boot-selected disposable backing device. No user pointer arguments.
int64_t sys_brook_pageable_memory(uint64_t operation,uint64_t value,
                                  uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    auto* actor=ProcessCurrent();KernelOperationGuard guard(actor);
    if(a||b||c||d)return -EINVAL;
    return PageableMemoryControl(actor,operation,value);
}

// ---------------------------------------------------------------------------
// sys_brook_profile (500) — Brook-specific: start/stop the sampling profiler
// ---------------------------------------------------------------------------
// arg0: op — 0 = start (arg1 = durationMs, 0 = indefinite), 1 = stop,
//            2 = isRunning (returns 1/0)
// Returns 0 on success, -EINVAL on bad op.

int64_t sys_brook_profile(uint64_t op, uint64_t a1, uint64_t, uint64_t,
                            uint64_t, uint64_t)
{
    switch (op) {
    case 0: brook::ProfilerStart(static_cast<uint32_t>(a1)); return 0;
    case 1: brook::ProfilerStop(); return 0;
    case 2: return brook::ProfilerIsRunning() ? 1 : 0;
#if defined(BROOK_GPU_SUBMISSION_TIMING) && !defined(BROOK_HOST_TEST)
    case 3: return a1 <= 4000 ? brook::ProfilerStartFrameTrace(static_cast<uint32_t>(a1), ProcessCurrent()) : -EINVAL;
#endif
    default: return -EINVAL;
    }
}

// sys_brook_set_crash_entry (502) — register the user-mode crash-dump writer
// entry point. One-shot: cannot be re-registered on the same leader.  The
// fault handler (commit d) will CreateRemoteThread(proc, crashEntry, ...)
// with a CrashCtx snapshot when a synchronous fatal exception hits this
// process.  See files/crash-dump-plan.md.
// ---------------------------------------------------------------------------
// arg0: entry (user VA of __brook_crash_entry)
// Returns 0 on success, -errno otherwise.

int64_t sys_brook_set_crash_entry(uint64_t entry,
                                   uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t)
{
    if (!entry) return -EINVAL;

    brook::Process* caller = brook::ProcessCurrent();
    if (!caller) return -ESRCH;

    brook::Process* leader = caller->threadLeader ? caller->threadLeader : caller;

    if (leader->crashEntry != 0) return -EINVAL; // already registered
    leader->crashEntry = entry;
    return 0;
}

// ---------------------------------------------------------------------------
// sys_brook_crash_complete (503) — called by the user-mode writer once the
// dump file has been flushed.  Terminates the whole thread group with the
// supplied exit code (conventionally 128 + signum).  This is the "don't
// come back" path — if the writer couldn't finish, the fault handler's
// hardKill fallback would have been taken instead.
// ---------------------------------------------------------------------------
// arg0: exitCode (int, stored in lower 8 bits of wait-status)
// Does not return on success.

int64_t sys_brook_crash_complete(uint64_t exitCode,
                                  uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t)
{
    brook::Process* caller = brook::ProcessCurrent();
    if (!caller) return -ESRCH;

    brook::Process* leader = caller->threadLeader ? caller->threadLeader : caller;

    SerialPrintf("crash_complete: tgid %u exiting with code %lu\n",
                 leader->tgid, exitCode);

    // Kill sibling threads (and the leader) with the crash exit code so
    // wait4 in the parent sees "exited with 128+signum" rather than
    // "killed by SIGKILL".
    SchedulerKillThreadGroup(leader->tgid, caller, static_cast<int>(exitCode));

    leader->crashInProgress = false;

    SchedulerExitCurrentProcess(static_cast<int>(exitCode));
    return 0; // unreachable
}

// ---------------------------------------------------------------------------
// sys_brook_input_pop (504) — drain the calling process's per-process input
// for the focused window into proc->inputQueue.  This syscall lets a
// userspace display server (waylandd) drain the queue without going via the
// /dev/keyboard fd path (which is scancode-only and lossy for mouse).
//
// Each event is delivered as 16 bytes:
//   [0]  type (uint8)   — InputEventType
//   [1]  scanCode (uint8)
//   [2]  ascii (int8)
//   [3]  modifiers (uint8)
//   [4..7] reserved
//   [8..11] mouse_x (int32) — only valid for MouseMove; absolute screen X
//   [12..15] mouse_y (int32) — only valid for MouseMove; absolute screen Y
// arg0: user buffer pointer
// arg1: max event count
// Returns: number of events copied, or -EFAULT on bad pointer.
// ---------------------------------------------------------------------------

extern "C" void MouseGetPosition(int32_t*, int32_t*);

static void EncodeInputPopEvent(Process* proc, InputEvent ev, uint8_t* slot)
{
    if(ev.type==InputEventType::MouseRelativeMove)ev.type=InputEventType::MouseMove;
    slot[0] = static_cast<uint8_t>(ev.type);
    slot[1] = ev.scanCode;
    slot[2] = static_cast<uint8_t>(ev.ascii);
    slot[3] = ev.modifiers;
    slot[4] = slot[5] = slot[6] = slot[7] = 0;
    int32_t mx = 0, my = 0;
    if (ev.type == InputEventType::MouseMove ||
        ev.type == InputEventType::MouseButtonDown ||
        ev.type == InputEventType::MouseButtonUp ||
        ev.type == InputEventType::MouseScroll) {
        int32_t sx = 0, sy = 0;
        MouseGetPosition(&sx, &sy);
        // Translate screen coords to caller's VFB coords. The compositor
        // blits the VFB to (fbDestX, fbDestY) at fbScale×; the inverse is
        // (sx - fbDestX) / fbScale.  Userspace then converts VFB→surface.
        uint8_t scale = proc->fbScale ? proc->fbScale : 1;
        mx = (sx - proc->fbDestX) / scale;
        my = (sy - proc->fbDestY) / scale;
    }
    memcpy(slot + 8, &mx, sizeof(mx));
    memcpy(slot + 12, &my, sizeof(my));
}

int64_t sys_brook_input_pop(uint64_t bufAddr, uint64_t maxCount,
                              uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!bufAddr) return -EFAULT;
    if (maxCount == 0) return 0;
    if (maxCount > 256) maxCount = 256;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    // Check the whole destination before popping, so a bad buffer does not
    // discard queued input. Events are then staged in kernel memory and
    // copied out with the fault-tolerant user copy.
    if (!UserBufferWritable(bufAddr, maxCount * 16)) return -EFAULT;
    static constexpr uint64_t Batch = 32;
    uint8_t staged[Batch * 16];
    uint64_t n = 0;
    bool drained = false;
    while (n < maxCount && !drained) {
        uint64_t batch = 0;
        while (batch < Batch && n + batch < maxCount) {
            InputEvent ev{};
            if (!ProcessInputPop(proc, &ev)) { drained = true; break; }
            EncodeInputPopEvent(proc, ev, staged + batch * 16);
            ++batch;
        }
        if (!batch) break;
        // A raced unmap loses only this batch; report what already landed.
        if (!CopyToUser(bufAddr + n * 16, staged, batch * 16))
            return n ? static_cast<int64_t>(n) : -EFAULT;
        n += batch;
    }
    return static_cast<int64_t>(n);
}

// ---------------------------------------------------------------------------
// sys_brook_input_grab (505) — register / unregister the calling process as
// the global input grabber.  When set, the compositor copies every keyboard
// and mouse event into the grabber's per-PID input queue (in addition to the
// kernel WM's per-window routing), so a userspace Wayland compositor can
// fan input out to its own clients via wl_pointer / wl_keyboard.
//   arg0 = enable (1 to grab, 0 to release)
//   returns 0 on success, -EPERM if releasing while not the current grabber.
// ---------------------------------------------------------------------------
int64_t sys_brook_input_grab(uint64_t enable, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    bool ok = brook::CompositorSetInputGrabber(proc, enable != 0);
    return ok ? 0 : -EPERM;
}

// Global desktop master gain, including native ALSA/OSS applications.
int64_t sys_brook_audio_control(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return brook::AudioOutputControl();
    if(op==2)return brook::AudioAvailable()?1:0;
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return brook::AudioSetUserOutputControl(static_cast<uint32_t>(value))?0:-EINVAL;
}

// Boot-opted-in read-only export of the single initialized AHCI disk. Disk
// data is copied to kernel-owned memory before touching the user buffer, so
// neither the AHCI mutex nor its DMA buffer spans a user-memory fault.
int64_t sys_brook_ahci_export(uint64_t op,uint64_t destination,uint64_t length,uint64_t offset,uint64_t r0,uint64_t r1)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    if(!VfsCanAccessSystemFiles() ||
       __atomic_load_n(&proc->mountNamespaceMember,__ATOMIC_ACQUIRE)!=VfsMountNamespaceInitial())return -EPERM;
    KernelOperationGuard operation(proc);
    if(r0 || r1)return -EINVAL;
    if(op==0) {
        if(offset || length!=sizeof(BrookAhciExportInfo))return -EINVAL;
        if(!destination)return -EFAULT;
        BrookAhciExportInfo info{};if(!brook::AhciReadOnlyExportInfo(info))return -30;
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==1) {
        if(!length || length>BROOK_AHCI_EXPORT_CHUNK || (length&511) || (offset&511))return -EINVAL;
        if(!destination)return -EFAULT;
        void* staged=kmalloc(uint32_t(length));if(!staged)return -ENOMEM;
        const int read=brook::AhciReadOnlyExportRead(offset,staged,uint32_t(length));
        int64_t result=read;
        if(read>0 && !CopyToUser(destination,staged,uint32_t(read)))result=-EFAULT;
        kfree(staged);return result;
    }
    return -EINVAL;
}

} // namespace brook
