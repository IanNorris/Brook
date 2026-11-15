#include "../../shared/brook_pointer.h"
#include "mouse.h"
#include "process_fs.h"
#include "seek_position.h"
#include "guard_checker_session.h"
#if defined(BROOK_SSD_GUARD_REPAIR)
#include "guard_repair_session.h"
#endif
#include "device.h"
#include "ahci_system_volume.h"
#include "../../shared/brook_system_esp.h"
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
#include "strict_gpt_inventory.h"
#endif
#include "kprintf.h"
#include "mount_namespace.h"
#include "tmpfs.h"
#include "tmpfs_mount_options.h"
#include "process_user_namespace.h"
#include "unix_lifetime_trace.h"
#include "file_locks.h"
#include "memfd_seals.h"
#include "drm_resource_layout.h"
// syscall.cpp -- SYSCALL/SYSRET dispatcher, syscall table, and user-mode entry.
//
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "syscall.h"
#include "robust_futex.h"
#include "posix_timer.h"
#include "sysv_sem.h"
#include "signal_fd.h"
#include "signal_altstack.h"
#include "unix_credentials.h"
#include "timeout_value.h"
#include "timerfd_state.h"
#include "dsp_geometry.h"
#include "futex_timeout.h"
#include "sync_file.h"
#include "gpu_output_abi.h"
#include "gpu_buffer_abi.h"
#include "gpu_window_source_abi.h"
#include "desktop_layers_export.h"
#include "gpu_reservation.h"
#include "busybox_applets.h"
#include "cpu.h"
#include "fw_cfg.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"
#include "process.h"
#include "system_message.h"
#include "user_access_scope.h"
#include "desktop_identity.h"
#include "exec_limits.h"
#include "drm_map_tokens.h"
#include "drm_backing_diagnostics.h"
#include "intel_bdw_buffer.h"
#ifdef BROOK_BDW_CACHE_PROBE
#include "../../shared/bdw_cache_probe_abi.h"
#endif
#include "intel_bdw_file.h"
#include "intel_bdw_ioctl.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_scanout_target.h"
#include "intel_bdw_refusal.h"
#include "debug_hardware.h"
#include "sync/ksemaphore.h"
#include "exec_image.h"
#include "fpu_irq.h"
#include "scheduler.h"
#include "spinlock.h"
#include "sync/futex_waiter_pool.h"
#include "sync/pi_wait_graph.h"
#include "signal_restart.h"
#include "sync/kmutex.h"
#include "string.h"
#include "memory/virtual_memory.h"
#include "memory/mapping_transaction.h"
#include "memory/file_page_cache.h"
#include "memory/file_object.h"
#include "memory/user_mapping.h"
#include "memory/mapping_fault.h"
#include "memory/pageable_memory.h"
#include "memory/mapping_fault_test.h"
#include "register_test.h"
#include "memory/physical_memory.h"
#include "vfs.h"
#include "ext2_vfs.h"
#include "tty.h"
#include "input.h"
#include "pipe.h"
#include "memory/heap.h"
#include "memory/object_cache.h"
#include "epoll_diagnostics.h"
#include "compositor.h"
#include "../../shared/brook_debug_frame.h"
#include "ahci_readonly.h"
#include "compositor_scene.h"
#include "wm_chrome_key.h"
#include "window.h"
#include "gpu_app.h"
#include "gpu_compositor.h"
#include "terminal.h"
#include "net.h"
#include "rtc.h"
#include "kvmclock.h"
#include "tsc_clock.h"
#include "audio.h"
#include "hda_observe.h"
#include "dsp_write_plan.h"
#include "dsp_producer_timing.h"
#include "syscall_timing.h"
#include "profiler.h"
#include "smp.h"
#include "apic.h"
#include "serial_writer.h"
#include "debug_overlay.h"
#include "user_copy.h"
#include "gs_paranoid.h"

#include "syscall_internal.h"
#include "syscall_unix_socket.h"
#include "syscall_drm.h"
#include "syscall_memory.h"
#include "syscall_handlers.h"

// Forward declaration
extern "C" __attribute__((naked)) void ReturnToKernel();
extern "C" bool MemFdHandleUserFault(uint64_t cr2, uint64_t errCode);
extern "C" bool FileMapHandleUserFault(uint64_t cr2, uint64_t errCode);

using brook::UserBufferReadable;
using brook::UserBufferWritable;
using brook::CopyToUser;
using brook::CopyUserCString;

// ---------------------------------------------------------------------------
// C dispatch wrapper — reads syscall number from GS:120, applies strace.
// Must be extern "C" for the assembly call instruction.
// ---------------------------------------------------------------------------

namespace brook { int64_t SyscallDispatchInternal(uint64_t num, uint64_t a0, uint64_t a1,
                                                   uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5); }

extern "C" int64_t SyscallDispatchC(uint64_t a0, uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t num;
    asm volatile("movq %%gs:120, %0" : "=r"(num));

    return brook::SyscallDispatchInternal(num, a0, a1, a2, a3, a4, a5);
}

extern "C" void SyscallSnapshotCurrentC()
{
    uint64_t procPtr;
    uint64_t userRip, userRsp, userRflags;
    uint64_t userRbx, userRbp, userR12, userR13, userR14, userR15;
    uint64_t userRdi, userRsi, userRdx, userR8, userR9, userR10;
    uint64_t syscallNum;

    asm volatile("movq %%gs:184, %0" : "=r"(procPtr));
    asm volatile("movq %%gs:48,  %0" : "=r"(userRip));
    asm volatile("movq %%gs:56,  %0" : "=r"(userRsp));
    asm volatile("movq %%gs:64,  %0" : "=r"(userRflags));
    asm volatile("movq %%gs:72,  %0" : "=r"(userRbx));
    asm volatile("movq %%gs:80,  %0" : "=r"(userRbp));
    asm volatile("movq %%gs:88,  %0" : "=r"(userR12));
    asm volatile("movq %%gs:96,  %0" : "=r"(userR13));
    asm volatile("movq %%gs:104, %0" : "=r"(userR14));
    asm volatile("movq %%gs:112, %0" : "=r"(userR15));
    asm volatile("movq %%gs:128, %0" : "=r"(userRdi));
    asm volatile("movq %%gs:136, %0" : "=r"(userRsi));
    asm volatile("movq %%gs:144, %0" : "=r"(userRdx));
    asm volatile("movq %%gs:152, %0" : "=r"(userR8));
    asm volatile("movq %%gs:160, %0" : "=r"(userR9));
    asm volatile("movq %%gs:168, %0" : "=r"(userR10));
    asm volatile("movq %%gs:120, %0" : "=r"(syscallNum));

    auto* proc = reinterpret_cast<brook::Process*>(procPtr);
    if (!proc)
        return;

    proc->syscallUserRip = userRip;
    proc->syscallUserRsp = userRsp;
    proc->syscallUserRflags = userRflags;
    proc->syscallUserRbx = userRbx;
    proc->syscallUserRbp = userRbp;
    proc->syscallUserR12 = userR12;
    proc->syscallUserR13 = userR13;
    proc->syscallUserR14 = userR14;
    proc->syscallUserR15 = userR15;
    proc->syscallUserRdi = userRdi;
    proc->syscallUserRsi = userRsi;
    proc->syscallUserRdx = userRdx;
    proc->syscallUserR8 = userR8;
    proc->syscallUserR9 = userR9;
    proc->syscallUserR10 = userR10;
    proc->currentSyscallNum = syscallNum;
}

// ---------------------------------------------------------------------------
// SYSCALL dispatcher -- naked assembly, pointed to by LSTAR MSR.
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked, used)) void BrookSyscallDispatcher()
{
    __asm__ volatile(
        "swapgs\n\t"
        "movq %%r11, %%gs:24\n\t"      // stash user RFLAGS
        "mov %%rsp, %%r11\n\t"          // r11 = user RSP
        "mov %%gs:8, %%rsp\n\t"
        "and $~0xF, %%rsp\n\t"
        // Keep all existing frame/RBP offsets and 16-byte call alignment.
        "sub $64, %%rsp\n\t"
        "movq $0, (%%rsp)\n\t"          // this entry defaults to SYSRET
        "push %%r11\n\t"               // [1] user RSP
        "push %%rcx\n\t"               // [2] user return address
        "movq %%gs:24, %%r11\n\t"       // reload user RFLAGS
        "push %%r11\n\t"               // [3] user RFLAGS
        "push %%rbp\n\t"               // [4] user RBP
        "mov %%rsp, %%rbp\n\t"
        "push %%rdx\n\t"               // [5]
        "push %%rsi\n\t"               // [6]
        "push %%rdi\n\t"               // [7]
        "push %%r8\n\t"                // [8]
        "push %%r9\n\t"                // [9]
        "push %%r10\n\t"               // [10]
        "push %%r11\n\t"               // [11]
        "push %%r12\n\t"               // [12]
        "push %%r13\n\t"               // [13]
        "push %%r14\n\t"               // [14]
        "push %%r15\n\t"               // [15]
        "push %%rbx\n\t"               // [16] — 16 pushes = 128 bytes, aligned
        "mov %%r10, %%rcx\n\t"
        "movq %%rax, %%gs:120\n\t"       // save syscall number for debug
        "mov %%gs:16, %%r12\n\t"
        // Save user context in KernelCpuEnv for fork().
        // Use R15 as temp (saved at [rbp-88]).
        "mov 16(%%rbp), %%r15\n\t"       // user RCX (return addr)
        "movq %%r15, %%gs:48\n\t"        // -> syscallUserRip
        "mov 24(%%rbp), %%r15\n\t"       // user RSP
        "movq %%r15, %%gs:56\n\t"        // -> syscallUserRsp
        "mov 8(%%rbp), %%r15\n\t"        // user RFLAGS (R11)
        "movq %%r15, %%gs:64\n\t"        // -> syscallUserRflags
        // Save callee-saved registers for fork child
        "mov -96(%%rbp), %%r15\n\t"      // RBX at [rbp-96]
        "movq %%r15, %%gs:72\n\t"        // -> syscallUserRbx
        "mov 0(%%rbp), %%r15\n\t"        // user RBP at [rbp+0]
        "movq %%r15, %%gs:80\n\t"        // -> syscallUserRbp
        "mov -64(%%rbp), %%r15\n\t"      // R12 at [rbp-64]
        "movq %%r15, %%gs:88\n\t"        // -> syscallUserR12
        "mov -72(%%rbp), %%r15\n\t"      // R13 at [rbp-72]
        "movq %%r15, %%gs:96\n\t"        // -> syscallUserR13
        "mov -80(%%rbp), %%r15\n\t"      // R14 at [rbp-80]
        "movq %%r15, %%gs:104\n\t"       // -> syscallUserR14
        "mov -88(%%rbp), %%r15\n\t"      // R15 at [rbp-88]
        "movq %%r15, %%gs:112\n\t"       // -> syscallUserR15
        // Caller-saved registers (needed for fork — Linux preserves all regs)
        "mov -24(%%rbp), %%r15\n\t"      // RDI at [rbp-24]
        "movq %%r15, %%gs:128\n\t"       // -> syscallUserRdi
        "mov -16(%%rbp), %%r15\n\t"      // RSI at [rbp-16]
        "movq %%r15, %%gs:136\n\t"       // -> syscallUserRsi
        "mov -8(%%rbp), %%r15\n\t"       // RDX at [rbp-8]
        "movq %%r15, %%gs:144\n\t"       // -> syscallUserRdx
        "mov -32(%%rbp), %%r15\n\t"      // R8 at [rbp-32]
        "movq %%r15, %%gs:152\n\t"       // -> syscallUserR8
        "mov -40(%%rbp), %%r15\n\t"      // R9 at [rbp-40]
        "movq %%r15, %%gs:160\n\t"       // -> syscallUserR9
        "mov -48(%%rbp), %%r15\n\t"      // R10 at [rbp-48]
        "movq %%r15, %%gs:168\n\t"       // -> syscallUserR10
        // Syscalls preserve user vector state even though the C handlers may
        // use SSE. Reuse the IRQ stack-save convention before the first C
        // call: R14 retains the unchanged GPR-frame base, R15 the 64-byte
        // aligned XSAVE area. Both are callee-saved across blocking switches.
        // At most 1215 additional bytes are reserved on the 256 KiB stack;
        // the save area sits above the C stack and below the GPR frame.
        BROOK_FPU_SAVE_IRQ
        "call SyscallSnapshotCurrentC\n\t"
        "mov -24(%%rbp), %%rdi\n\t"      // a0
        "mov -16(%%rbp), %%rsi\n\t"      // a1
        "mov -8(%%rbp), %%rdx\n\t"       // a2
        "mov -48(%%rbp), %%rcx\n\t"      // a3 (user R10)
        "mov -32(%%rbp), %%r8\n\t"       // a4
        "mov -40(%%rbp), %%r9\n\t"       // a5
        "sti\n\t"
        "call SyscallDispatchC\n\t"
        "cli\n\t"
        // Check for pending signals before returning to userspace.
        // Pass the saved-register frame and syscall return value.
        "mov %%r14, %%rdi\n\t"         // original SyscallFrame, above XSAVE area
        "mov %%rax, %%rsi\n\t"         // RSI = syscall result
        "mov %%r15, %%rdx\n\t"        // authoritative entry XSAVE slot
        "call SyscallCheckSignals\n\t"  // returns new RAX
        "jmp .Lsyscall_return\n\t"
        ".Lsyscall_invalid:\n\t"
        "mov $-38, %%rax\n\t"
        ".Lsyscall_return:\n\t"
        // XRSTOR uses EDX:EAX as its mask. Preserve the final syscall result
        // (including rt_sigreturn's restored RAX) in a saved scratch register.
        "mov %%rax, %%r13\n\t"
        BROOK_FPU_RESTORE_IRQ
        "mov %%r13, %%rax\n\t"
        "pop %%rbx\n\t"                // [16]
        "pop %%r15\n\t"                // [15]
        "pop %%r14\n\t"                // [14]
        "pop %%r13\n\t"                // [13]
        "pop %%r12\n\t"                // [12]
        "pop %%r11\n\t"                // [11]
        "pop %%r10\n\t"                // [10]
        "pop %%r9\n\t"                 // [9]
        "pop %%r8\n\t"                 // [8]
        "pop %%rdi\n\t"                // [7]
        "pop %%rsi\n\t"                // [6]
        "pop %%rdx\n\t"                // [5]
        "pop %%rbp\n\t"                // [4]
        // RSP is now frame+104. Signal return must restore RCX and R11 as
        // ordinary GPRs; SYSRET would replace them with RIP and RFLAGS.
        "cmpq $0, 24(%%rsp)\n\t"        // returnState.mode at frame+128
        "jne .Lsigreturn_iret\n\t"
        "pop %%r11\n\t"                // [3] user RFLAGS
        "pop %%rcx\n\t"                // [2] user return address
        // Every high bit must be clear for Brook's low-47-bit user range.
        // Bit 47 alone misses noncanonical values with higher bits set.
        // The scratch push reuses the consumed RIP slot, preserving RAX/RSP.
        "push %%rax\n\t"
        "mov %%rcx, %%rax\n\t"
        "shr $47, %%rax\n\t"
        "test %%rax, %%rax\n\t"
        "pop %%rax\n\t"
        "jnz .Lsysret_bad_rcx\n\t"
        "swapgs\n\t"
        "mov (%%rsp), %%rsp\n\t"       // [1] user RSP
        ".byte 0x48\n\t"
        "sysret\n\t"
        ".Lsysret_bad_rcx:\n\t"
        "mov $0x3F8, %%dx\n\t"
        "mov $0x58, %%al\n\t"
        "outb %%al, (%%dx)\n\t"
        "int3\n\t"
        "cli\n\t"
        ".Lsysret_halt:\n\t"
        "hlt\n\t"
        "jmp .Lsysret_halt\n\t"
        ".Lsigreturn_iret:\n\t"
        "mov 32(%%rsp), %%rcx\n\t"      // frame+136, original signal RCX
        "add $40, %%rsp\n\t"            // frame+144, validated IRET frame
        IRETQ_NORMALIZE_RFLAGS
        "swapgs\n\t"
        "iretq\n\t"
        ::: "memory"
    );
}
namespace brook {

// Read-only per-user retained system events. A failed user copy never consumes
// an event; the caller advances its own cursor only after receiving the record.
static int64_t sys_brook_system_message(uint64_t after,uint64_t output,uint64_t size,
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
static int64_t sys_brook_pageable_memory(uint64_t operation,uint64_t value,
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

static int64_t sys_brook_profile(uint64_t op, uint64_t a1, uint64_t, uint64_t,
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

static int64_t sys_brook_set_crash_entry(uint64_t entry,
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

static int64_t sys_brook_crash_complete(uint64_t exitCode,
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

static int64_t sys_brook_input_pop(uint64_t bufAddr, uint64_t maxCount,
                                     uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!bufAddr) return -EFAULT;
    if (maxCount == 0) return 0;
    if (maxCount > 256) maxCount = 256;

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;

    auto* out = reinterpret_cast<uint8_t*>(bufAddr);
    uint64_t n = 0;
    while (n < maxCount) {
        InputEvent ev{};
        if (!ProcessInputPop(proc, &ev)) break;
        if(ev.type==InputEventType::MouseRelativeMove)ev.type=InputEventType::MouseMove;
        uint8_t* slot = out + (n * 16);
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
        *reinterpret_cast<int32_t*>(slot + 8)  = mx;
        *reinterpret_cast<int32_t*>(slot + 12) = my;
        ++n;
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
static int64_t sys_brook_input_grab(uint64_t enable, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    bool ok = brook::CompositorSetInputGrabber(proc, enable != 0);
    return ok ? 0 : -EPERM;
}

// ---------------------------------------------------------------------------
// Brook WM syscalls 506-509 — multi-window-per-process WM API.
// Phase A of the wayland↔WM unification.  Lets one process (e.g. waylandd)
// own multiple top-level windows, each backed by its own kernel-allocated
// VFB mapped read/write into the caller's address space.
// ---------------------------------------------------------------------------

// 506: WM_CREATE_WINDOW
//   arg0 = clientW (uint16)
//   arg1 = clientH (uint16)
//   arg2 = title pointer (user, 0-terminated)
//   arg3 = out struct pointer (user) — receives:
//          uint32_t wmId, uint32_t vfbStride, uint64_t vfbUser
//   arg4 = flags:
//          bit 0 requests initial client-side decoration/noChrome
//          bit 1 creates a raised but non-keyboard-focusable popup/tool window
//          bit 2 identifies this process itself as the native application client
// Returns 0 on success, -errno on failure.
struct BrookWmCreateOut {
    uint32_t wmId;
    uint32_t vfbStride;
    uint64_t vfbUser;
};

static int64_t sys_brook_wm_create_window(uint64_t cW, uint64_t cH,
                                           uint64_t titlePtr, uint64_t outPtr,
                                           uint64_t flags, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (cW == 0 || cH == 0 || cW > 0xFFFF || cH > 0xFFFF) return -EINVAL;
    if (!outPtr) return -EFAULT;

    char titleBuf[64] = {};
    if (titlePtr) {
        const char* utp = reinterpret_cast<const char*>(titlePtr);
        for (uint32_t i = 0; i < sizeof(titleBuf) - 1; ++i) {
            char c = utp[i];
            titleBuf[i] = c;
            if (!c) break;
        }
        titleBuf[sizeof(titleBuf) - 1] = '\0';
    }

    if (flags & ~7ull) return -EINVAL;
    bool focusable = (flags & 2u) == 0;
    auto res = brook::WmCreateWindowForProcess(proc,
                                                 static_cast<uint16_t>(cW),
                                                 static_cast<uint16_t>(cH),
                                                 titlePtr ? titleBuf : nullptr,
                                                 focusable, (flags & 4u) != 0);
    if (res.wmId == 0) return -ENOMEM;
    if (flags & 1u) {
        brook::WmSetClientSideDecoration(static_cast<int>(res.wmId) - 1, true);
    }

    auto* uo = reinterpret_cast<BrookWmCreateOut*>(outPtr);
    uo->wmId      = res.wmId;
    uo->vfbStride = res.vfbStride;
    uo->vfbUser   = reinterpret_cast<uint64_t>(res.vfbUser);
    return 0;
}

// 507: WM_DESTROY_WINDOW(wmId)
static int64_t sys_brook_wm_destroy_window(uint64_t wmId, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFFFFFu) return -EINVAL;
    brook::WmDestroyWindowById(proc, static_cast<uint32_t>(wmId));
    return 0;
}

// 508: WM_SIGNAL_DIRTY(wmId) — caller has finished writing to the VFB.
static int64_t sys_brook_wm_signal_dirty(uint64_t wmId, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0) return -EINVAL;
    brook::WmSignalDirtyById(proc, static_cast<uint32_t>(wmId));
    return 0;
}

static int64_t sys_brook_wm_publish_source_commit(uint64_t wmId,uint64_t incarnation,uint64_t commit,
                                                   uint64_t a,uint64_t b,uint64_t c)
{
    Process* proc=ProcessCurrent();
    if(!proc)return -ESRCH;
    if(!wmId || wmId>0xffffffffULL || a || b || c)return -EINVAL;
    return brook::WmPublishSourceCommit(proc,static_cast<uint32_t>(wmId),incarnation,commit);
}
static int64_t sys_brook_wm_begin_source_write(uint64_t wmId,uint64_t incarnation,uint64_t a,
                                                uint64_t b,uint64_t c,uint64_t d)
{
    Process* proc=ProcessCurrent();if(!proc)return -ESRCH;
    if(!wmId || wmId>0xffffffffULL || a || b || c || d)return -EINVAL;
    return brook::WmBeginSourceWrite(proc,static_cast<uint32_t>(wmId),incarnation);
}
static int64_t sys_brook_wm_query_source_commit(uint64_t wmId,uint64_t incarnation,uint64_t generation,
                                                 uint64_t output,uint64_t a,uint64_t b)
{
    if(!wmId || wmId>0xffffffffULL || !output || a || b || !UserBufferWritable(output,sizeof(uint64_t)))return -EINVAL;
    uint64_t commit=0;const auto result=brook::WmQuerySourceCommit(static_cast<uint32_t>(wmId),incarnation,generation,&commit);
    if(result)return result;
    return CopyToUser(output,&commit,sizeof(commit))?0:-EFAULT;
}

// 525: WM_PRESENT_GRES(wmId, gres, w, h) — present a hardware-GL (dmabuf) frame.
// waylandd resolves a client's dmabuf to a host virgl resource id (gres) and
// calls this to wait for a compositor-owned capture of the source frame.
// The gres must be one the caller legitimately holds (it came from the
// caller importing a PRIME fd the client sent it over the Wayland socket).
static int64_t sys_brook_wm_present_gres(uint64_t wmId, uint64_t gres,
                                         uint64_t w, uint64_t h,
                                         uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    constexpr uint64_t wireMax=0xffffffffULL;
    if (!wmId || wmId>wireMax || !gres || gres>wireMax || !w || !h ||
        w>wireMax || h>wireMax) return -EINVAL;
    // This legacy ABI names a resource numerically, but the caller must own
    // its GEM capability. In particular, never permit an unexported resource
    // to become an untracked compositor source before enabling reclamation.
    MappingBackingGuard pin;
    DrmResource* resource=nullptr;
    for (uint32_t fd=0;fd<MAX_FDS && !resource;++fd) {
        FdRef descriptor(proc,static_cast<int>(fd));
        if (!descriptor || descriptor->type!=FdType::DevDri || !descriptor->handle) continue;
        auto* context=static_cast<DrmCtx*>(descriptor->handle);
        DrmMappingGuard guard(context);
        for (auto& gem:context->gem) {
            if (!gem.used || gem.resource->gres!=gres) continue;
            if (!__atomic_load_n(&gem.resource->everExported,__ATOMIC_ACQUIRE)) return -EINVAL;
            resource=gem.resource;MappingBackingRef(resource->lifetime);
            pin.value=resource->lifetime;break;
        }
    }
    if (!resource || resource->owner->config.capsetId==4 || resource->target!=2 || !brook::GpuCaptureFormatSupported(resource->format) ||
        w>resource->width || h>resource->height) return -EINVAL;
    brook::WmCaptureToken token{};
    if (!brook::WmReserveCapture(proc,static_cast<uint32_t>(wmId),
                               static_cast<uint32_t>(w),static_cast<uint32_t>(h),&token)) return -ENOENT;
    return brook::CompositorCaptureExternal(proc,token,resource->lifetime,
                                           resource->owner->ctxId,resource->gres,resource->format);
}

static int64_t sys_brook_wm_place_window(uint64_t window, uint64_t incarnation,
                                         uint64_t x, uint64_t y, uint64_t flags, uint64_t reserved)
{
    auto* proc=ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    const int64_t clientX=static_cast<int64_t>(x), clientY=static_cast<int64_t>(y);
    if (!window || window>WM_MAX_WINDOWS || !incarnation || flags || reserved ||
        clientX < -32768 || clientX > 32767 || clientY < -32768 || clientY > 32767) return -EINVAL;
    return brook::WmPlaceOwnedWindow(proc,static_cast<uint32_t>(window),incarnation,
                                     static_cast<int32_t>(clientX),static_cast<int32_t>(clientY));
}

static int64_t sys_brook_wm_window_state(uint64_t window, uint64_t incarnation,
                                         uint64_t output, uint64_t size,
                                         uint64_t flags, uint64_t reserved)
{
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (!window || window > WM_MAX_WINDOWS || size != sizeof(BrookWindowState) || flags || reserved)
        return -EINVAL;
    // Prefault outside the window lock. Copy a local snapshot after unlocking,
    // so user faults never run under the compositor's window mutex.
    if (!UserBufferWritable(output, sizeof(BrookWindowState))) return -EFAULT;
    BrookWindowState state{};
    const int result = brook::WmSnapshotOwnedWindowState(proc, static_cast<uint32_t>(window), incarnation, &state);
    if (result) return result;
    return CopyToUser(output, &state, sizeof(state)) ? 0 : -EFAULT;
}

static_assert(BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_WM_APPEARANCE &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_WM_TASKBAR_METRICS &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_AUDIO_CONTROL &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_DEBUG_FRAME &&
              BROOK_SYS_GPU_OUTPUT_ALLOC!=BROOK_SYS_AHCI_EXPORT,
              "GPU output syscall must not overlap existing Brook extensions");
static int64_t sys_brook_gpu_output_alloc(uint64_t bytes,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(!bytes || bytes>BROOK_GPU_OUTPUT_MAX_BYTES || flags || a || b || c || d)return -EINVAL;
    if(!brook::BdwRenderExperimentEnabled())return -ENODEV;
    auto* buffer=brook::BdwGpuBufferCreateHandoff(bytes);if(!buffer)return -ENOMEM;
    FdEntry owned{};owned.type=FdType::IntelPrime;owned.handle=buffer;owned.fdFlags=1;
    const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
    if(fd<0)brook::BdwGpuBufferUnref(buffer);
    return fd;
#else
    (void)bytes;(void)flags;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

static_assert(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ==0xB1B && BROOK_SYS_GPU_BUFFER_TRY_CPU_READ!=BROOK_SYS_DESKTOP_OVERLAY,
              "GPU shared-buffer syscall must be distinct");
static int64_t sys_brook_gpu_buffer_try_cpu_read(uint64_t fd,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(flags || a || b || c || d)return -EINVAL;
    if(fd>=MAX_FDS)return -EBADF;
    FdRef ref(proc,static_cast<int>(fd));if(!ref)return -EBADF;
    auto* entry=ref.get();
    if(entry->type!=FdType::IntelPrime || !entry->handle)return -95; // ENOTSUP
    auto* buffer=static_cast<brook::BdwGpuBuffer*>(entry->handle);
    if(!brook::BdwGpuBufferAllowsUserMapping(buffer))return -13; // EACCES
    const auto result=brook::BdwGpuBufferTryCpuTransition(buffer);
    if(result==brook::BdwCpuTransitionResult::Ready)return 0;
    return result==brook::BdwCpuTransitionResult::Busy?-EAGAIN:-EIO;
#else
    (void)fd;(void)flags;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

static int64_t sys_brook_gpu_buffer_snapshot_fence(uint64_t fd,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(flags || a || b || c || d)return -EINVAL;
    if(fd>=MAX_FDS)return -EBADF;
    FdRef ref(proc,static_cast<int>(fd));if(!ref)return -EBADF;
    auto* entry=ref.get();if(entry->type!=FdType::IntelPrime || !entry->handle)return -95;
    auto* buffer=static_cast<brook::BdwGpuBuffer*>(entry->handle);
    if(!brook::BdwGpuBufferAllowsUserMapping(buffer))return -13;
    brook::BdwFence* fence=nullptr;
    const auto result=brook::BdwAcquireApplicationBufferFence(buffer,&fence);if(result)return result;
    const int published=CreateIntelSyncFile(proc,fence);brook::BdwFenceUnref(fence);
    if(published<0)return published;
    SyncFilePublicationPin pin(proc,published);return published;
#else
    (void)fd;(void)flags;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

static_assert(BROOK_SYS_GPU_HANDOFF_CTL!=BROOK_SYS_GPU_OUTPUT_ALLOC && BROOK_SYS_GPU_HANDOFF_CTL==0xB18,
              "GPU handoff syscall must be distinct");
static int64_t sys_brook_gpu_handoff_ctl(uint64_t op,uint64_t arg,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if((a && op!=BROOK_GPU_HANDOFF_RESERVE_DEPENDENT) || b || c || d)return -EINVAL;
    const uint64_t owner=proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation;
    if(op==BROOK_GPU_HANDOFF_PRESENT_CAPS)
        return arg?-EINVAL:(BROOK_GPU_PRESENT_CAP_FRAME|BROOK_GPU_PRESENT_CAP_ASYNC|BROOK_GPU_PRESENT_CAP_DEPENDENT
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
            |BROOK_GPU_PRESENT_CAP_SCANOUT
#endif
            );
#if defined(BROOK_BDW_ZERO_COPY_SCANOUT)
    if(op==BROOK_GPU_HANDOFF_SCANOUT_REGISTER){
        BrookGpuScanoutImages wire{};
        if(!CopyFromUser(&wire,arg,sizeof(wire)))return -EFAULT;
        if(wire.reserved || !wire.count || wire.count>BROOK_GPU_SCANOUT_MAX_IMAGES)return -EINVAL;
        static_assert(BROOK_GPU_SCANOUT_MAX_IMAGES==brook::BdwZeroCopyMaxImages,"zero-copy image limits must match");
        // Hold a buffer reference (not the fd) across registration.
        brook::BdwGpuBuffer* buffers[BROOK_GPU_SCANOUT_MAX_IMAGES]{};
        auto drop=[&](){for(auto* b:buffers)brook::BdwGpuBufferUnref(b);};
        for(uint32_t i=0;i<wire.count;++i){
            if(wire.fds[i]<0 || uint32_t(wire.fds[i])>=uint32_t(MAX_FDS)){drop();return -EBADF;}
            FdRef ref(proc,wire.fds[i]);
            if(!ref || ref->type!=FdType::IntelPrime || !ref->handle){drop();return -EBADF;}
            buffers[i]=static_cast<brook::BdwGpuBuffer*>(ref->handle);brook::BdwGpuBufferRef(buffers[i]);
        }
        brook::BdwZeroCopyRefusal refusal{};
        const brook::BdwZeroCopyGeometry geometry{wire.width,wire.height,wire.stride,wire.fourcc};
        const int64_t result=brook::BdwZeroCopyRegister(owner,buffers,wire.count,geometry,refusal);
        drop();return result;
    }
    if(op==BROOK_GPU_HANDOFF_SCANOUT_ADOPT){
        if(!UserBufferWritable(arg,sizeof(BrookGpuScanoutImages)))return -EFAULT;
        brook::BdwGpuBuffer* images[BROOK_GPU_SCANOUT_MAX_IMAGES]{};uint32_t count=0;
        const int64_t adopted=brook::BdwZeroCopyAdopt(owner,images,count);
        if(adopted)return adopted==-2?-ENOENT:adopted==-1?-EPERM:adopted;
        BrookGpuScanoutImages wire{count,brook::BdwZeroCopyWidth,brook::BdwZeroCopyHeight,brook::BdwZeroCopyStride,
            BROOK_GPU_SCANOUT_FOURCC_XRGB8888,0,{-1,-1,-1,-1}};
        int64_t result=0;
        for(uint32_t i=0;i<count;++i){
            if(result){brook::BdwGpuBufferUnref(images[i]);continue;}
            FdEntry owned{};owned.type=FdType::IntelPrime;owned.handle=images[i];owned.fdFlags=1;
            const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
            if(fd<0){brook::BdwGpuBufferUnref(images[i]);result=fd;continue;}
            wire.fds[i]=fd; // the fd table now holds the reference
        }
        if(!result && !CopyToUser(arg,&wire,sizeof(wire)))result=-EFAULT;
        if(result)for(uint32_t i=0;i<count;++i)if(wire.fds[i]>=0)(void)CloseProcessFd(proc,wire.fds[i]);
        return result;
    }
    if(op==BROOK_GPU_HANDOFF_SCANOUT_STATUS){
        brook::BdwZeroCopyStatus s{};brook::BdwZeroCopyReadStatus(s);
        BrookGpuScanoutStatus wire{};
        wire.flags=(s.registered?BROOK_GPU_SCANOUT_REGISTERED:0u)|(s.requested?BROOK_GPU_SCANOUT_REQUESTED:0u)|
            (s.active?BROOK_GPU_SCANOUT_ACTIVE:0u)|(s.disabled?BROOK_GPU_SCANOUT_DISABLED:0u);
        wire.images=s.images;wire.freeMask=s.freeMask;wire.front=s.front;wire.armed=s.armed;wire.queued=s.queued;
        wire.refusal=uint32_t(s.refusal);wire.sequence=s.sequence;wire.presents=s.stats.presents;
        wire.latched=s.stats.latched;wire.missedVblanks=s.stats.missedVblanks;
        if(!UserBufferWritable(arg,sizeof(wire)) || !CopyToUser(arg,&wire,sizeof(wire)))return -EFAULT;
        return 0;
    }
#endif
    if(op==BROOK_GPU_HANDOFF_BUFFER_FAILED){
        if(arg>=MAX_FDS)return -EBADF;
        FdRef ref(proc,static_cast<int>(arg));
        if(!ref || ref->type!=FdType::IntelPrime || !ref->handle)return -EBADF;
        return brook::BdwGpuBufferGpuFailed(static_cast<brook::BdwGpuBuffer*>(ref->handle))?1:0;
    }
    if(op==BROOK_GPU_HANDOFF_PRESENT_RESULT)return brook::BdwOffscreenPresentResult(owner,arg);
    if(op==BROOK_GPU_HANDOFF_PRESENT_FRAME || op==BROOK_GPU_HANDOFF_QUEUE_FRAME){
        BrookGpuPresentFrame wire{};
        if(!CopyFromUser(&wire,arg,sizeof(wire)))return -EFAULT;
        if(op==BROOK_GPU_HANDOFF_QUEUE_FRAME){
            if(wire.reserved)return -EINVAL;
            return brook::BdwQueueOffscreenPresent(owner,wire.ticket,wire.width,wire.height,wire.stride,nullptr,0);
        }
        BrookGpuPresentProbe frame{wire.ticket,0,0,wire.width,wire.height,wire.stride,wire.reserved,{}};
        return brook::BdwOffscreenHandoffPresentProbe(owner,frame,true,false,false);
    }
    if(op==BROOK_GPU_HANDOFF_PRESENT_REGIONS || op==BROOK_GPU_HANDOFF_QUEUE_REGIONS){
        BrookGpuPresentRegions wire{};
        if(!CopyFromUser(&wire,arg,sizeof(wire)))return -EFAULT;
        static_assert(BROOK_GPU_PRESENT_MAX_REGIONS==brook::COMPOSITOR_MAX_REGIONS,"region limits must match");
        if(!wire.ticket || wire.reserved || !wire.count || wire.count>BROOK_GPU_PRESENT_MAX_REGIONS)return -EINVAL;
        brook::CompositorPresentRegion regions[brook::COMPOSITOR_MAX_REGIONS]{};
        for(uint32_t i=0;i<wire.count;++i){const auto& r=wire.regions[i];
            if(r.reserved)return -EINVAL;
            regions[i]={r.x,r.y,r.width,r.height,r.stride,r.offset};
        }
        if(op==BROOK_GPU_HANDOFF_QUEUE_REGIONS)
            return brook::BdwQueueOffscreenPresent(owner,wire.ticket,0,0,0,regions,wire.count);
        return brook::BdwOffscreenHandoffPresentRegions(owner,wire.ticket,regions,wire.count);
    }
    if(op==BROOK_GPU_HANDOFF_PRESENT_PROBE || op==BROOK_GPU_HANDOFF_PRESENT_LEASE){
        BrookGpuPresentProbe probe{};
        if(!CopyFromUser(&probe,arg,sizeof(probe)) || !UserBufferWritable(arg,sizeof(probe)))return -EFAULT;
        const int64_t result=brook::BdwOffscreenHandoffPresentProbe(owner,probe,op==BROOK_GPU_HANDOFF_PRESENT_LEASE);
        if(result)return result;
        return CopyToUser(arg,&probe,sizeof(probe))?0:-EFAULT;
    }
    if(op==BROOK_GPU_HANDOFF_RESERVE || op==BROOK_GPU_HANDOFF_RESERVE_DEPENDENT){
        if(arg>=MAX_FDS)return -EBADF;
        FdRef ref(proc,static_cast<int>(arg));
        if(!ref || ref->type!=FdType::IntelPrime || !ref->handle)return -EBADF;
        const int64_t result=brook::BdwOffscreenHandoffControl(owner,op,arg,static_cast<brook::BdwGpuBuffer*>(ref->handle),a);
        if(result<0 && result!=-116){ // -116 (ESTALE): a dependent baseline moved, the renderer's normal retry
            static uint64_t refusals=0;const char* why=brook::BdwTakeRefusal();
            if(brook::BdwRefusalLogDue(refusals))
                KPrintf("gpu-handoff: reserve refused pid=%u errno=%ld reason=%s count=%lu\n",proc->pid,-result,why?why:"unrecorded",refusals);
        }else (void)brook::BdwTakeRefusal();
        return result;
    }
    return brook::BdwOffscreenHandoffControl(owner,op,arg,nullptr);
#else
    (void)op;(void)arg;(void)a;(void)b;(void)c;(void)d;return -ENODEV;
#endif
}

static_assert(BROOK_SYS_DESKTOP_LAYERS!=BROOK_SYS_GPU_HANDOFF_CTL && BROOK_SYS_DESKTOP_LAYERS!=BROOK_SYS_GPU_OUTPUT_ALLOC,
              "desktop layers syscall must be distinct");
WmChromeCaptureStats g_chromeCaptureStats{}; // window.h / wm_chrome_key.h
DesktopCaptureCounters g_desktopCaptureStats{}; // desktop_layers_export.h
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
static int64_t CaptureDesktopChrome(uint64_t owner,uint64_t op,uint64_t address,uint64_t size)
{
    auto* wire=static_cast<BrookDesktopLayers*>(kmalloc(sizeof(BrookDesktopLayers)));
    auto* snapshot=static_cast<brook::WmDesktopLayers*>(kmalloc(sizeof(brook::WmDesktopLayers)));
    if(!wire || !snapshot){kfree(wire);kfree(snapshot);return -ENOMEM;}
    int64_t result=-EFAULT;
    const bool keep=op==BROOK_DESKTOP_CHROME_TRACKED_KEEP;
    const bool keyed=op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_KEYED;
    if(CopyFromUser(wire,address,sizeof(*wire)) && UserBufferWritable(address,sizeof(*wire))){
        const uint64_t heldKey=wire->reserved;
        if(wire->version!=BROOK_DESKTOP_LAYERS_VERSION || (keep?!heldKey:wire->reserved!=0) || !wire->layers[0].window)result=-EINVAL;
        else if(!brook::WmAcquireDesktopLayers(snapshot))result=-95;
        else {
            auto& layer=wire->layers[0];uint32_t index=0;
            while(index<snapshot->count && snapshot->layers[index].window!=layer.window)++index;
            if(index==snapshot->count)result=-ENOENT;
            else {
                brook::WmDesktopChrome chrome{};
                const bool tracked=op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE || keyed || keep;
                int32_t keyMouseX=-1,keyMouseY=-1;
                if(keep)MouseGetPosition(&keyMouseX,&keyMouseY);
                result=brook::WmDesktopChromeCapture(*snapshot,index,&chrome,nullptr,0,keyMouseX,keyMouseY,tracked);
                const uint64_t bytes=uint64_t(chrome.width)*chrome.height*4;
                const bool capture=op!=BROOK_DESKTOP_CHROME_QUERY && !keep;
                const auto& source=snapshot->layers[index];
                uint64_t renderedKey=0;
                if(!result && keep){
                    // Unchanged chrome: the caller's buffer already holds these
                    // exact pixels. Anything else (content, size, hover, focus,
                    // title, theme) means recapture.
                    if(wire->count!=1 || layer.incarnation!=source.incarnation || layer.width!=chrome.width ||
                       layer.height!=chrome.height || layer.bytes!=bytes || chrome.key!=heldKey){
                        __atomic_add_fetch(&brook::g_chromeCaptureStats.keepMisses,1,__ATOMIC_RELAXED);result=-116;
                    }else if(!brook::WmDesktopLayersCaptureCurrent(*snapshot))result=-EAGAIN;
                    else {
                        renderedKey=heldKey;
                        __atomic_add_fetch(&brook::g_chromeCaptureStats.keeps,1,__ATOMIC_RELAXED);
                        __atomic_add_fetch(&brook::g_chromeCaptureStats.bytesKept,bytes,__ATOMIC_RELAXED);
                    }
                }
                if(!result && capture && (wire->count!=1 || layer.incarnation!=source.incarnation ||
                   (!tracked && layer.generation!=source.generation) || layer.width!=chrome.width || layer.height!=chrome.height ||
                   layer.clientX!=chrome.x || layer.clientY!=chrome.y || layer.bytes!=bytes))result=-EAGAIN;
                if(!result && capture && bytes && (!layer.pixels || layer.capacity<bytes || layer.pixels>~uint64_t{0}-bytes ||
                   (layer.pixels<address+size && address<layer.pixels+bytes)))result=-EINVAL;
                uint32_t* pixels=nullptr;
                if(!result && capture && bytes){
                    pixels=static_cast<uint32_t*>(kmalloc(bytes));
                    if(!pixels)result=-ENOMEM;
                    else {
                        brook::WmDesktopChrome current{};
                        int32_t mouseX=0,mouseY=0;MouseGetPosition(&mouseX,&mouseY);
                        result=brook::WmDesktopChromeCapture(*snapshot,index,&current,pixels,bytes,mouseX,mouseY,tracked);
                        if(!result && (current.x!=chrome.x || current.y!=chrome.y || current.width!=chrome.width || current.height!=chrome.height))result=-EAGAIN;
                        if(!result && !CopyToUser(layer.pixels,pixels,bytes))result=-EFAULT;
                        if(!result){
                            renderedKey=current.key;
                            __atomic_add_fetch(&brook::g_chromeCaptureStats.captures,1,__ATOMIC_RELAXED);
                            __atomic_add_fetch(&brook::g_chromeCaptureStats.bytesCopied,bytes,__ATOMIC_RELAXED);
                        }
                        if(!result && !(tracked?brook::WmDesktopLayersCaptureCurrent(*snapshot):brook::WmDesktopLayersCurrent(*snapshot)))result=-EAGAIN;
                    }
                }
                kfree(pixels);
                if(!result && (op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE || keyed || keep))
                    result=brook::BdwOffscreenRegisterChrome(owner,source);
                if(!result){
                    layer.incarnation=source.incarnation;layer.generation=source.generation;layer.bytes=bytes;
                    layer.width=layer.stride=layer.clientWidth=chrome.width;layer.height=layer.clientHeight=chrome.height;
                    layer.clientX=chrome.x;layer.clientY=chrome.y;layer.upscale=1;layer.flags=0;wire->count=1;
                    // A chrome-less window has no pixels; its key is still
                    // meaningful (0 bytes) so the caller can keep it too.
                    wire->reserved=(keyed||keep)?(renderedKey?renderedKey:chrome.key):0;
                    if(!CopyToUser(address,wire,sizeof(*wire)))result=-EFAULT;
                }
            }
            brook::WmReleaseDesktopLayers(snapshot);
        }
    }
    kfree(snapshot);kfree(wire);return result;
}
static int64_t CaptureDesktopBackdrop(uint64_t owner,uint64_t op,uint64_t address,uint64_t size)
{
    auto* wire=static_cast<BrookDesktopLayers*>(kmalloc(sizeof(BrookDesktopLayers)));
    auto* scratch=static_cast<uint32_t*>(kmalloc(4096));
    if(!wire || !scratch){kfree(wire);kfree(scratch);return -ENOMEM;}
    int64_t result=-EFAULT;
    if(CopyFromUser(wire,address,sizeof(*wire)) && UserBufferWritable(address,sizeof(*wire))){
        brook::CompositorBackdropInfo info{};
        if(wire->version!=BROOK_DESKTOP_LAYERS_VERSION || wire->reserved)result=-EINVAL;
        else if(!brook::CompositorGetBackdropInfo(&info))result=-ENODEV;
        else if(uint64_t(info.width)*info.height>64ull*1024*1024/4)result=-E2BIG;
        else {
            auto& layer=wire->layers[0];const uint64_t bytes=uint64_t(info.width)*info.height*4;
            const bool capture=op==BROOK_DESKTOP_BACKDROP_CAPTURE ||
                op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE;
            result=op==BROOK_DESKTOP_BACKDROP_TRACKED_BIND?
                brook::BdwOffscreenRegisterBackdrop(owner,info):0;
            if(capture && (wire->count!=1 || layer.incarnation!=info.display || layer.generation!=info.generation ||
               layer.width!=info.width || layer.height!=info.height || layer.stride!=info.width || layer.bytes!=bytes))result=-EAGAIN;
            else if(capture && (!layer.pixels || layer.capacity<bytes || layer.pixels>~uint64_t{0}-bytes ||
                    (layer.pixels<address+size && address<layer.pixels+bytes)))result=-EINVAL;
            if(!result && capture){
                for(uint32_t y=0;y<info.height && !result;++y)for(uint32_t x=0;x<info.width;){
                    const uint32_t count=info.width-x<1024?info.width-x:1024;
                    if(!brook::CompositorReadBackdropSpan(info,y,x,count,scratch)){result=-EAGAIN;break;}
                    if(!CopyToUser(layer.pixels+(uint64_t(y)*info.width+x)*4,scratch,uint64_t(count)*4)){result=-EFAULT;break;}
                    x+=count;
                }
                brook::CompositorBackdropInfo after{};
                if(!result && (!brook::CompositorGetBackdropInfo(&after) || after.display!=info.display ||
                   after.generation!=info.generation || after.width!=info.width || after.height!=info.height))result=-EAGAIN;
                if(!result && op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE)
                    result=brook::BdwOffscreenRegisterBackdrop(owner,info);
            }
            if(!result){
                layer.incarnation=info.display;layer.generation=info.generation;layer.bytes=bytes;
                layer.width=layer.stride=layer.clientWidth=info.width;layer.height=layer.clientHeight=info.height;
                layer.window=0;layer.clientX=layer.clientY=0;layer.upscale=1;layer.flags=0;wire->count=1;
                if(!CopyToUser(address,wire,sizeof(*wire)))result=-EFAULT;
            }
        }
    }
    kfree(scratch);kfree(wire);return result;
}
#endif
static int64_t sys_brook_desktop_layers(uint64_t op,uint64_t address,uint64_t size,uint64_t a,uint64_t b,uint64_t c)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(!brook::BdwRenderExperimentEnabled())return -ENODEV;
    if(op==BROOK_DESKTOP_CLIENT_FRAME){
        if(address || size || !a || a>BROOK_DESKTOP_LAYER_LIMIT || !b || !c)return -EINVAL;
        brook::WmNoteClientFrame(b,c);return 0;
    }
    if((op==BROOK_DESKTOP_CHROME_QUERY || op==BROOK_DESKTOP_CHROME_CAPTURE || op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE ||
        op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_KEYED || op==BROOK_DESKTOP_CHROME_TRACKED_KEEP) &&
       size==sizeof(BrookDesktopLayers) && !a && !b && !c && address<=~uint64_t{0}-size)
        return CaptureDesktopChrome(proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation,op,address,size);
    if((op==BROOK_DESKTOP_BACKDROP_QUERY || op==BROOK_DESKTOP_BACKDROP_CAPTURE ||
        op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE || op==BROOK_DESKTOP_BACKDROP_TRACKED_BIND) &&
       size==sizeof(BrookDesktopLayers) && !a && !b && !c && address<=~uint64_t{0}-size)
        return CaptureDesktopBackdrop(proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation,op,address,size);
    const bool keepOp=op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_KEEP;
    const bool tracked=op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED || op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE || keepOp;
    if((op!=BROOK_DESKTOP_LAYERS_QUERY && op!=BROOK_DESKTOP_LAYERS_CAPTURE && !tracked) ||
       size!=sizeof(BrookDesktopLayers) || (op!=BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE && !keepOp && a) ||
       a>0xffffffffULL || (!keepOp && b) || b>0xffffffffULL || c || address>~uint64_t{0}-size)return -EINVAL;
    const bool capture=op!=BROOK_DESKTOP_LAYERS_QUERY;
    auto* wire=static_cast<BrookDesktopLayers*>(kmalloc(sizeof(BrookDesktopLayers)));
    auto* layers=static_cast<brook::WmDesktopLayers*>(kmalloc(sizeof(brook::WmDesktopLayers)));
    uint64_t scratchBytes=65536;
    auto* scratch=capture?static_cast<uint8_t*>(kmalloc(scratchBytes)):nullptr;
    if(capture && !scratch){scratchBytes=4096;scratch=static_cast<uint8_t*>(kmalloc(scratchBytes));}
    if(!wire || !layers || (capture && !scratch)){
        kfree(wire);kfree(layers);kfree(scratch);return -ENOMEM;
    }
    int64_t result=-EFAULT;
    if(CopyFromUser(wire,address,sizeof(*wire)) && UserBufferWritable(address,sizeof(*wire))){
        if(!brook::WmAcquireDesktopLayers(layers))result=-95;
        else {
            auto sink=[&](const brook::WmDesktopLayer& layer,uint64_t offset,uint64_t destination,uint64_t bytes){
                // Never let pixel output overwrite the request/metadata.
                if(destination<address+size && address<destination+bytes)return false;
                if(!brook::UserMappingBufferRead(layer.backing,offset,scratch,bytes))return false;
                brook::DesktopInvertCaret(layer,offset,scratch,bytes);
                return CopyToUser(destination,scratch,bytes);
            };
            brook::DesktopCaptureCounters tally{};
            result=brook::ExportDesktopLayers(*layers,*wire,capture,sink,
                [tracked](const brook::WmDesktopLayers& current){
                    return tracked
                        ?brook::WmDesktopLayersCaptureCurrent(current)
                        :brook::WmDesktopLayersCurrent(current);
                },scratchBytes,static_cast<uint32_t>(a),static_cast<uint32_t>(b),&tally);
            if(!result && tally.calls){auto& g=brook::g_desktopCaptureStats;
                __atomic_add_fetch(&g.calls,tally.calls,__ATOMIC_RELAXED);__atomic_add_fetch(&g.keeps,tally.keeps,__ATOMIC_RELAXED);
                __atomic_add_fetch(&g.keepMisses,tally.keepMisses,__ATOMIC_RELAXED);__atomic_add_fetch(&g.captures,tally.captures,__ATOMIC_RELAXED);
                __atomic_add_fetch(&g.bytesCopied,tally.bytesCopied,__ATOMIC_RELAXED);__atomic_add_fetch(&g.bytesKept,tally.bytesKept,__ATOMIC_RELAXED);}
            bool adopted=false;
            if(!result && tracked){
                const uint64_t owner=proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation;
                result=brook::BdwOffscreenRegisterScene(owner,layers,&wire->reserved);adopted=!result;
            }
            if(!result && !CopyToUser(address,wire,sizeof(*wire)))result=-EFAULT;
            if(!adopted)brook::WmReleaseDesktopLayers(layers);
            else layers=nullptr;
        }
    }
    kfree(scratch);kfree(layers);kfree(wire);return result;
#else
    (void)op;(void)address;(void)size;(void)a;(void)b;(void)c;return -ENODEV;
#endif
}

// 0xB06/0xB07: capability-based retained scenes. See gpu_scene_abi.h.
static int64_t sys_brook_scene_create(uint64_t window,uint64_t abi,uint64_t flags,
                                      uint64_t a,uint64_t b,uint64_t c)
{
    auto* proc=ProcessCurrent();if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (!window || window>WM_MAX_WINDOWS || abi!=SCENE_ABI_VERSION || flags || a || b || c) return -EINVAL;
    int error=0;auto* handle=SceneCreate(proc,static_cast<uint32_t>(window),&error);
    if (!handle) return error;
    FdEntry owned{};owned.type=FdType::CompositorScene;owned.handle=handle;owned.fdFlags=1;
    SceneHandleRef(handle); // FD publication owns a reference independently of this call.
    const int fd=FdTablePublishOwned(proc->fds,ProcessFdLock(proc),owned,0,-1,nullptr);
    if (fd<0) { SceneHandleUnref(handle);SceneHandleUnref(handle);return fd; }
    const bool activated=SceneActivate(handle);
    if (!activated) {
        // The published slot may already have been closed/reused by a sibling.
        // Retain the constructor handle while comparing, then pin the exact
        // slot so rollback cannot close an unrelated replacement descriptor.
        FdRef published(proc,fd);
        if (published && published->type==FdType::CompositorScene && published->handle==handle)
            CloseProcessFd(proc,fd);
        SceneHandleUnref(handle);return -116;
    }
    SceneHandleUnref(handle);
    return fd;
}
static int64_t sys_brook_scene_ctl(uint64_t fd,uint64_t op,uint64_t arg,uint64_t size,
                                   uint64_t flags,uint64_t reserved)
{
    auto* proc=ProcessCurrent();if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (fd>=MAX_FDS) return -EBADF;
    if (flags || reserved) return -EINVAL;
    FdRef ref(proc,static_cast<int>(fd));
    if (!ref || ref->type!=FdType::CompositorScene || !ref->handle) return -EBADF;
    auto* handle=static_cast<SceneHandle*>(ref->handle);
    if (op==SCENE_CTL_STATS) {
        if (size!=sizeof(SceneStatsDesc)) return -EINVAL;
        SceneStatsDesc stats{};SceneGetStats(handle,stats);
        return CopyToUser(arg,&stats,sizeof(stats))?0:-EFAULT;
    }
    if (op==SCENE_CTL_RELEASE) {
        if (size) return -EINVAL;
        return SceneRelease(handle,arg);
    }
    if (op==SCENE_CTL_UPLOAD) {
        if (size!=sizeof(SceneUploadDesc)) return -EINVAL;
        SceneUploadDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        return SceneUpload(proc,handle,desc);
    }
    if (op==SCENE_CTL_CAPTURE) {
        if (size!=sizeof(SceneCaptureDesc)) return -EINVAL;
        SceneCaptureDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved) return -EINVAL;
        FdRef drm(proc,desc.drmFd);
        if (!drm || drm->type!=FdType::DevDri || !drm->handle) return -EBADF;
        MappingBackingGuard lease;
        int32_t producer;uint32_t resource,format;
        {
            auto* context=static_cast<DrmCtx*>(drm->handle);DrmMappingGuard guard(context);
            auto* gem=DrmGemGet(context,desc.gem);
            if (!gem || gem->resource->owner->config.capsetId==4 || gem->resource->target!=2 || !GpuCaptureFormatSupported(gem->resource->format) ||
                desc.width>gem->resource->width || desc.height>gem->resource->height) return -EINVAL;
            auto* image=gem->resource;MappingBackingRef(image->lifetime);lease.value=image->lifetime;
            producer=image->owner->ctxId;resource=image->gres;format=image->format;
        }
        return SceneCapture(proc,handle,desc,lease.value,producer,resource,format);
    }
    if (op==SCENE_CTL_CAPTURE_DMABUF) {
        if(size!=sizeof(SceneDmabufCaptureDesc))return -EINVAL;
        SceneDmabufCaptureDesc desc{};if(!CopyFromUser(&desc,arg,sizeof(desc)))return -EFAULT;
        const BrookDmabufLayout layout{desc.width,desc.height,desc.fourcc,desc.stride,desc.offset,desc.flags,desc.modifier};
        FdRef drm(proc,desc.drmFd);
        if(!drm || drm->type!=FdType::DevDri || !drm->handle)return -EBADF;
        MappingBackingGuard lease;DrmResource* image=nullptr;
        {
            auto* context=static_cast<DrmCtx*>(drm->handle);DrmMappingGuard guard(context);
            auto* gem=DrmGemGet(context,desc.gem);if(!gem)return -EINVAL;
            image=gem->resource;
            if(!__atomic_load_n(&image->everExported,__ATOMIC_ACQUIRE))return -EINVAL;
            const bool venus=image->owner->config.capsetId==4;
            if(BrookDmabufValidate(&layout,image->size,!venus)!=BROOK_DMABUF_LAYOUT_OK)return -EINVAL;
            if(!venus && (image->target!=2 || desc.width!=image->width || desc.height!=image->height ||
               desc.stride!=image->stride || desc.offset ||
               !GpuCaptureViewSupported(image->format,desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u))) {
                SerialPrintf("drm: rejected image view: target=%u backing=%ux%u stride=%u format=%u view=%ux%u stride=%u fourcc=%x offset=%u\n",
                             image->target,image->width,image->height,image->stride,image->format,
                             desc.width,desc.height,desc.stride,desc.fourcc,desc.offset);
                return -EINVAL;
            }
            if(venus && (!image->hostBlob || !(image->blobFlags&2u)))return -EINVAL;
            MappingBackingRef(image->lifetime);lease.value=image->lifetime;
        }
        const bool venus=image->owner->config.capsetId==4;
        if(venus) {
            // Only the current frame's imported WRITE fence is evidence of
            // Venus queue completion. A prior capture or ring0 ack is not.
            KMutexLock(&image->reservationLock);
            const bool admitted=image->reservation.BeginCapture();
            KMutexUnlock(&image->reservationLock);
            if(!admitted)return -11; // EAGAIN, producer synchronization absent
        }
        const SceneCaptureDesc capture{desc.drmFd,desc.gem,desc.width,desc.height,
                                      desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u,0};
        // VirGL BGRA/BGRX values, distinct from the scene's alpha policy.
        const uint32_t format=venus?(desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u):image->format;
        return SceneCapture(proc,handle,capture,lease.value,image->owner->ctxId,image->gres,format,
                            venus?&layout:nullptr,venus?SceneCaptureSync{image,DrmCaptureComplete}:SceneCaptureSync{});
    }
    if (op==SCENE_CTL_CLONE) {
        if (size!=sizeof(SceneCloneDesc)) return -EINVAL;
        SceneCloneDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved) return -EINVAL;
        if (desc.sourceFd<0 || static_cast<uint32_t>(desc.sourceFd)>=MAX_FDS) return -EBADF;
        FdRef source(proc,desc.sourceFd);
        if (!source || source->type!=FdType::CompositorScene || !source->handle) return -EBADF;
        return SceneClone(proc,handle,static_cast<SceneHandle*>(source->handle),desc.version);
    }
    if (op==SCENE_CTL_PRESENT) {
        if (size!=sizeof(ScenePresentDesc)) return -EINVAL;
        ScenePresentDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved || desc.count>GPU_SCENE_MAX_LAYERS) return -EINVAL;
        auto* layers=static_cast<SceneLayerDesc*>(kmalloc(sizeof(SceneLayerDesc)*GPU_SCENE_MAX_LAYERS));
        if (!layers) return -ENOMEM;
        if (desc.count && !CopyFromUser(layers,desc.layers,sizeof(SceneLayerDesc)*desc.count)) {
            kfree(layers);return -EFAULT;
        }
        const int64_t result=ScenePresent(proc,handle,desc,layers);kfree(layers);return result;
    }
    return -EINVAL;
}

// 0xB03: notify the desktop that installed application metadata changed.
static int64_t sys_brook_wm_refresh_apps(uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    brook::WmRequestLauncherRefresh();
    return 0;
}

// 0xB02: desktop theme query (op 0) or select immutable named palette (op 1).
static int64_t sys_brook_wm_theme(uint64_t op, uint64_t theme,
                                 uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    if (op == 0) return brook::WmGetTheme();
    if (op != 1 || theme > 0xffffffffULL) return -EINVAL;
    return brook::WmSetTheme(static_cast<uint32_t>(theme)) ? 0 : -EINVAL;
}

// 0xB0B: atomic appearance preferences, independent of material palette/scale.
static int64_t sys_brook_wm_appearance(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return WmGetAppearance();
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return WmSetAppearance(static_cast<uint32_t>(value))?0:-EINVAL;
}

// Global desktop master gain, including native ALSA/OSS applications.
static int64_t sys_brook_audio_control(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return brook::AudioOutputControl();
    if(op==2)return brook::AudioAvailable()?1:0;
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return brook::AudioSetUserOutputControl(static_cast<uint32_t>(value))?0:-EINVAL;
}

// 0xB0C: independently selectable taskbar telemetry widgets.
static int64_t sys_brook_pointer_speed(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return MouseTouchpadSpeed();
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return MouseTouchpadSetSpeed(static_cast<uint32_t>(value))?0:-EINVAL;
}
static int64_t sys_brook_wm_taskbar_metrics(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t){
    if(!ProcessCurrent())return -ESRCH;
    if(op==0)return WmGetTaskbarMetrics();
    if(op!=1||value>0xffffffffULL)return -EINVAL;
    return WmSetTaskbarMetrics(static_cast<uint32_t>(value))?0:-EINVAL;
}

// Root-only completed-frame copy. The compositor lock never spans user memory
// faults, network I/O or filesystem I/O. Process incarnation prevents PID reuse
// from inheriting another process's capture; abandoned leases expire boundedly.
static int64_t sys_brook_debug_frame(uint64_t op,uint64_t token,uint64_t destination,uint64_t length,uint64_t offset,uint64_t reserved)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    if(proc->euid!=0)return -EPERM;
    KernelOperationGuard operation(proc);
    if(reserved || token>0xffffffffULL)return -EINVAL;
    const uint64_t owner=proc->incarnation;
    if(op==0){
        if(token||destination||length||offset)return -EINVAL;
        const auto frame=CompositorRequestDebugFrame(owner);
        if(frame.state==DebugFrameState::Busy)return -16; // EBUSY
        return frame.token?int64_t(frame.token):-EIO;
    }
    if(op==1){
        if(offset || length!=sizeof(BrookDebugFrameInfo))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116; // ESTALE
        BrookDebugFrameInfo info{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==4){
        if(offset || length!=sizeof(BrookDebugFrameInfoV2))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116;
        BrookDebugFrameInfoV2 info{{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch},frame.source,0};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==5){
        if(offset || length!=sizeof(BrookDebugFrameInfoV3))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116;
        BrookDebugFrameInfoV3 info{{{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch},frame.source,0},frame.presentationMode,0};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==2){
        if(!length || length>BROOK_DEBUG_FRAME_CHUNK)return -EINVAL;
        void* staged=kmalloc(uint32_t(length));if(!staged)return -ENOMEM;
        int64_t result=CompositorReadDebugFrame(uint32_t(token),owner,offset,staged,uint32_t(length));
        if(result>0 && !CopyToUser(destination,staged,uint32_t(result)))result=-EFAULT;
        kfree(staged);return result;
    }
    if(op==3){
        if(destination||length||offset)return -EINVAL;
        return CompositorReleaseDebugFrame(uint32_t(token),owner)?0:-116; // ESTALE
    }
    return -EINVAL;
}

// Experimental renderer overlay uses the immutable compositor-thread frame
// lease; it shares the single capture slot with root debug screenshots.
static int64_t sys_brook_desktop_overlay(uint64_t op,uint64_t token,uint64_t destination,uint64_t length,uint64_t offset,uint64_t reserved)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(!brook::BdwRenderExperimentEnabled())return -ENODEV;
    if(reserved || token>0xffffffffULL)return -EINVAL;
    const uint64_t owner=proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation;
    if(op==0){
        if(token || destination || length || (offset & ~uint64_t(BROOK_DESKTOP_OVERLAY_NO_CURSOR)))return -EINVAL;
        const auto frame=CompositorRequestDebugFrame(owner,true,!(offset&BROOK_DESKTOP_OVERLAY_NO_CURSOR));
        if(frame.state==DebugFrameState::Busy)return -16;
        return frame.token?int64_t(frame.token):-EIO;
    }
    if(op==1){
        if(offset || length!=sizeof(BrookDebugFrameInfo))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116;
        BrookDebugFrameInfo info{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==2){
        if(!length || length>BROOK_DEBUG_FRAME_CHUNK)return -EINVAL;
        void* staged=kmalloc(uint32_t(length));if(!staged)return -ENOMEM;
        int64_t result=CompositorReadDebugFrame(uint32_t(token),owner,offset,staged,uint32_t(length));
        if(result>0 && !CopyToUser(destination,staged,uint32_t(result)))result=-EFAULT;
        kfree(staged);return result;
    }
    if(op==3){
        if(destination || length || offset)return -EINVAL;
        return CompositorReleaseDebugFrame(uint32_t(token),owner)?0:-116;
    }
    if(op==4){
        if(offset || length!=sizeof(BrookDesktopOverlayBands))return -EINVAL;
        DesktopOverlayBands bands{};const int result=CompositorOverlayBands(uint32_t(token),owner,bands);
        if(result)return result;
        BrookDesktopOverlayBands info{};info.count=bands.count;
        for(uint32_t i=0;i<bands.count;++i)info.bands[i]={bands.bands[i].x,bands.bands[i].y,bands.bands[i].width,bands.bands[i].height};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_OVERLAY_GENERATION){
        if(token || destination || length || offset)return -EINVAL;
        return int64_t(CompositorOverlayGeneration()&0x7fffffffffffffffULL);
    }
    if(op==BROOK_DESKTOP_OVERLAY_WAIT){
        if(length!=sizeof(BrookDesktopOverlayInfo) || offset>BROOK_DESKTOP_OVERLAY_WAIT_MAX_MS)return -EINVAL;
        DebugFrameSnapshot frame{};
        if(CompositorWaitDebugFrame(uint32_t(token),owner,uint32_t(offset),frame))return -116;
        BrookDesktopOverlayInfo info{};
        info.frame={frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch};
        info.generation=frame.state==DebugFrameState::Ready?frame.overlayGeneration:0;
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_OVERLAY_CURSOR){
        if(token || length!=sizeof(BrookDesktopOverlayCursor))return -EINVAL;
        auto* cursor=static_cast<BrookDesktopOverlayCursor*>(kmalloc(sizeof(BrookDesktopOverlayCursor)));
        if(!cursor)return -ENOMEM;
        CompositorOverlayCursor(*cursor,offset);
        const uint64_t bytes=cursor->serial==offset?offsetof(BrookDesktopOverlayCursor,pixels):sizeof(*cursor);
        const bool copied=CopyToUser(destination,cursor,bytes);
        kfree(cursor);return copied?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_OVERLAY_READ_BAND){
        DesktopOverlayBands bands{};int result=CompositorOverlayBands(uint32_t(token),owner,bands);
        if(result)return result;
        if(offset>=bands.count)return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state!=DebugFrameState::Ready)return -116;
        const auto& band=bands.bands[offset];const uint64_t bytes=uint64_t(band.width)*band.height*4;
        if(!destination || !bytes || bytes!=length || bytes>64ull*1024*1024 || destination>~uint64_t{0}-bytes)return -EINVAL;
        if(!UserBufferWritable(destination,bytes))return -EFAULT;
        auto* scratch=static_cast<uint8_t*>(kmalloc(4096));if(!scratch)return -ENOMEM;
        int64_t failure=-EFAULT;
        const bool copied=DesktopOverlayPackedRead(band,frame.width,frame.height,4096,
            [&](uint64_t from,uint64_t to,uint32_t n){
                const int64_t read=CompositorReadDebugFrame(uint32_t(token),owner,from,scratch,n);
                if(read!=n){failure=read<0?read:-EIO;return false;}
                if(!CopyToUser(destination+to,scratch,n)){failure=-EFAULT;return false;}
                return true;
            });
        kfree(scratch);return copied?int64_t(bytes):failure;
    }
    return -EINVAL;
#else
    (void)op;(void)token;(void)destination;(void)length;(void)offset;(void)reserved;return -ENODEV;
#endif
}

// Boot-opted-in read-only export of the single initialized AHCI disk. Disk
// data is copied to kernel-owned memory before touching the user buffer, so
// neither the AHCI mutex nor its DMA buffer spans a user-memory fault.
static int64_t sys_brook_ahci_export(uint64_t op,uint64_t destination,uint64_t length,uint64_t offset,uint64_t r0,uint64_t r1)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    if(proc->euid!=0)return -EPERM;
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

// 0xB04: native desktop scale. Query requested (0), request (1), query applied (2).
static int64_t sys_brook_wm_scale(uint64_t op,uint64_t scale,uint64_t,uint64_t,uint64_t,uint64_t)
{
    if (!ProcessCurrent()) return -ESRCH;
    if (op==0) return WmGetRequestedScale();
    if (op==2) return WmGetAppliedScale();
    if (op!=1 || scale>0xffffffffULL) return -EINVAL;
    return WmSetScale(static_cast<uint32_t>(scale)) ? 0 : -EINVAL;
}

// 0xB01: WM_SET_APP_ID(wmId, appId*) — metadata for an owned window.
// 0xB05: WM_SET_CLIENT_SOCKET(wmId, accepted AF_UNIX stream fd). The kernel
// authenticates connection origin; the provider supplies the surface association.
// This metadata grants no authority to signal another process.
static int64_t sys_brook_wm_set_client_socket(uint64_t wmId,uint64_t fdValue,
    uint64_t,uint64_t,uint64_t,uint64_t)
{
    Process* proc=ProcessCurrent();
    if (!proc) return -ESRCH;
    if (!wmId || wmId>0xffffffffULL || fdValue>0x7fffffffULL) return -EINVAL;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc,static_cast<int>(fdValue));
    auto* entry=descriptor.get();
    if (!entry) return -EBADF;
    if (entry->type!=FdType::UnixSocket || !entry->handle) return -88; // ENOTSOCK
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    UnixCredentials credentials{};uint64_t incarnation=0,launchId=0;
    SpinLockAcquire(&g_unixRegistryLock);
    const bool accepted=socket->state==UnixSocketData::State::Connected &&
                        socket->acceptedEndpoint && socket->socketType==1;
    if (accepted) {
        credentials=socket->peerCredentials;incarnation=socket->peerIncarnation;launchId=socket->peerLaunchId;
    }
    SpinLockRelease(&g_unixRegistryLock);
    if (!accepted) return -95; // EOPNOTSUPP
    if (credentials.pid<=0 || credentials.pid>65535 || !incarnation) return -ESRCH;
    return WmRegisterClientOrigin(proc,static_cast<uint32_t>(wmId),
        static_cast<uint16_t>(credentials.pid),incarnation,launchId)?0:-EPERM;
}

static int64_t sys_brook_gpu_window_validate(uint64_t fdValue,uint64_t window,uint64_t incarnation,
                                              uint64_t a,uint64_t b,uint64_t c)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(fdValue>=MAX_FDS || !window || window>WM_MAX_WINDOWS || !incarnation || a || b || c)return -EINVAL;
    FdRef descriptor(proc,static_cast<int>(fdValue));auto* entry=descriptor.get();
    if(!entry || entry->type!=FdType::UnixSocket || !entry->handle)return -EBADF;
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    UnixCredentials peer{};uint64_t peerIncarnation=0;
    SpinLockAcquire(&g_unixRegistryLock);
    const bool accepted=socket->state==UnixSocketData::State::Connected && socket->acceptedEndpoint && socket->socketType==5;
    if(accepted){peer=socket->peerCredentials;peerIncarnation=socket->peerIncarnation;}
    SpinLockRelease(&g_unixRegistryLock);
    if(!accepted || peer.pid<=0 || !peerIncarnation)return -EPERM;
    return brook::WmGpuSourceProviderMatches(static_cast<uint32_t>(window),incarnation,
                                             static_cast<uint32_t>(peer.pid),peerIncarnation)?0:-EPERM;
}

static int64_t sys_brook_gpu_window_peer_recovery(uint64_t fdValue,uint64_t op,uint64_t bufferFd,
                                                   uint64_t a,uint64_t b,uint64_t c)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc || !proc->fds)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(fdValue>=MAX_FDS || a || b || c || op>BROOK_GPU_WINDOW_PEER_DRAIN ||
       (op==BROOK_GPU_WINDOW_PEER_AUTH && bufferFd))return -EINVAL;
    FdRef descriptor(proc,static_cast<int>(fdValue));auto* entry=descriptor.get();
    if(!entry || entry->type!=FdType::UnixSocket || !entry->handle)return -EBADF;
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    UnixCredentials peer{};uint64_t peerIncarnation=0;bool authorized=false;
    SpinLockAcquire(&g_unixRegistryLock);
    const bool connected=socket->state==UnixSocketData::State::Connected && !socket->acceptedEndpoint && socket->socketType==5;
    if(connected){peer=socket->peerCredentials;peerIncarnation=socket->peerIncarnation;authorized=socket->gpuPeerAuthorized;}
    SpinLockRelease(&g_unixRegistryLock);
    if(!connected || peer.pid<=0 || peer.pid>65535 || !peerIncarnation)return -EPERM;
    if(op==BROOK_GPU_WINDOW_PEER_AUTH){
        if(!brook::BdwOffscreenRendererOwnedBy(peerIncarnation))return -EPERM;
        SpinLockAcquire(&g_unixRegistryLock);
        const bool same=socket->state==UnixSocketData::State::Connected && socket->peerIncarnation==peerIncarnation && socket->peerCredentials.pid==peer.pid;
        if(same)socket->gpuPeerAuthorized=true;
        SpinLockRelease(&g_unixRegistryLock);return same?0:-116; // ESTALE
    }
    if(!authorized)return -EPERM;
    if(bufferFd>=MAX_FDS)return -EBADF;
    FdRef source(proc,static_cast<int>(bufferFd));auto* bufferEntry=source.get();
    if(!bufferEntry || bufferEntry->type!=FdType::IntelPrime || !bufferEntry->handle)return -EBADF;
    auto* buffer=static_cast<brook::BdwGpuBuffer*>(bufferEntry->handle);
    if(!brook::BdwGpuBufferAllowsUserMapping(buffer))return -13;
    if(!brook::SchedulerThreadGroupGone(static_cast<uint16_t>(peer.pid),peerIncarnation))return -EAGAIN;
    const auto state=brook::BdwGpuBufferTryCpuTransition(buffer);
    return state==brook::BdwCpuTransitionResult::Ready?0:state==brook::BdwCpuTransitionResult::Busy?-EAGAIN:-EIO;
#else
    (void)fdValue;(void)op;(void)bufferFd;(void)a;(void)b;(void)c;return -ENODEV;
#endif
}

static int64_t sys_brook_wm_set_app_id(uint64_t wmId, uint64_t appIdPtr,
                                      uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (!wmId || wmId > 0xffffffffULL || !appIdPtr) return -EINVAL;
    char appId[128] = {};
    if (!CopyUserCString(appIdPtr, appId, sizeof(appId))) return -EFAULT;
    return brook::WmSetAppIdById(proc, static_cast<uint32_t>(wmId), appId) ? 0 : -ENOENT;
}

// 509: WM_SET_TITLE(wmId, title*)
static int64_t sys_brook_wm_set_title(uint64_t wmId, uint64_t titlePtr,
                                       uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || !titlePtr) return -EINVAL;
    char buf[64] = {};
    const char* utp = reinterpret_cast<const char*>(titlePtr);
    for (uint32_t i = 0; i < sizeof(buf) - 1; ++i) {
        char c = utp[i];
        buf[i] = c;
        if (!c) break;
    }
    buf[sizeof(buf) - 1] = '\0';
    {
        extern volatile uint64_t g_blockCacheHits;
        extern volatile uint64_t g_blockCacheMisses;
        SerialPrintf("[PROFILE] set_title t=%lums pid=%u '%s'"
                     " opens=%lu mmaps=%lu faults=%lu"
                     " cache_hit=%lu cache_miss=%lu preloaded=%lu"
                     " blkcache_hit=%lu blkcache_miss=%lu\n",
                     ApicTickCount(), proc->pid, buf,
                     g_profOpenCount, g_profMmapCount, g_profFaultCount,
                     g_profCacheHit, g_profCacheMiss, g_profPreloadPages,
                     g_blockCacheHits, g_blockCacheMisses);
    }
    brook::WmSetTitleById(proc, static_cast<uint32_t>(wmId), buf);
    return 0;
}

// 0xB0A: owned-window pointer lease. Query is independent of event delivery,
// so a full user queue cannot keep a revoked capture active in userspace.
static int64_t sys_brook_wm_pointer_capture(uint64_t wmId,uint64_t operation,uint64_t token,
                                           uint64_t,uint64_t,uint64_t) {
    Process* proc=ProcessCurrent();
    if(!proc)return -ESRCH;
    if(!wmId || wmId>WM_MAX_WINDOWS || operation>3 || token>0xffffffffu)return -EINVAL;
    KernelOperationGuard operationGuard(proc);
    return brook::WmPointerCapture(proc,static_cast<uint32_t>(wmId),
                                   static_cast<uint32_t>(operation),static_cast<uint32_t>(token));
}

// 510: WM_POP_INPUT(wmId, buf*, max) — drain per-window input queue.
// Each event written is a brook::Window::WmInputEvent (12 bytes).
// Returns number of events written, or -errno.
static int64_t sys_brook_wm_pop_input(uint64_t wmId, uint64_t bufPtr,
                                       uint64_t max, uint64_t,
                                       uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || !bufPtr || !max) return -EINVAL;
    if (max > 256) max = 256;
    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -ENOENT;
    auto* uo = reinterpret_cast<brook::Window::WmInputEvent*>(bufPtr);
    return static_cast<int64_t>(brook::WmInputPop(w, uo, static_cast<uint32_t>(max)));
}

// 511: WM_RESIZE_VFB(wmId, newW, newH, outPtr)
//   Reallocates the kernel VFB for an existing waylandd-hosted toplevel
//   to (newW × newH).  Old user mapping is unmapped and a fresh user
//   mapping is returned via outPtr (BrookWmCreateOut layout).  Should be
//   called by waylandd after the client commits a buffer at the size we
//   asked for via xdg_toplevel.configure.
static int64_t sys_brook_wm_resize_vfb(uint64_t wmId, uint64_t newW,
                                        uint64_t newH, uint64_t outPtr,
                                        uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFFFFFu) return -EINVAL;
    if (newW == 0 || newH == 0 || newW > 0xFFFF || newH > 0xFFFF) return -EINVAL;
    if (!outPtr) return -EFAULT;

    brook::WmCreateWindowResult res = {};
    int rc = brook::WmResizeVfbForProcess(proc, static_cast<uint32_t>(wmId),
                                           static_cast<uint16_t>(newW),
                                           static_cast<uint16_t>(newH),
                                           &res);
    if (rc != 0) return rc;

    auto* uo = reinterpret_cast<BrookWmCreateOut*>(outPtr);
    uo->wmId      = res.wmId;
    uo->vfbStride = res.vfbStride;
    uo->vfbUser   = reinterpret_cast<uint64_t>(res.vfbUser);
    return 0;
}

// 512: WM_SET_DECORATION_MODE(wmId, csdEnabled)
//   Toggle CSD (client-side decoration) mode for a waylandd-hosted window.
//   Used to honour zxdg_toplevel_decoration_v1.set_mode.  When enabled,
//   the kernel WM stops drawing chrome and the whole outer area is client.
static int64_t sys_brook_wm_set_decoration_mode(uint64_t wmId, uint64_t csd,
                                                 uint64_t, uint64_t,
                                                 uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    int idx = static_cast<int>(wmId) - 1;
    brook::WmSetClientSideDecoration(idx, csd != 0);
    return 0;
}

// 0xB00 (2816): WM_SET_WINDOW_PROPERTIES(wmId, mask, opacity, blurRadius)
//   Set per-window display properties. `mask` selects which to apply:
//     bit 0 (WM_PROP_OPACITY) -> opacity  (0..255, 255 = opaque)
//     bit 1 (WM_PROP_BLUR)    -> blurRadius (0 = off)
//   A high (0xB00) number deliberately clear of the Linux and Windows syscall
//   ranges. Replaces the opt/gpuopacity demo hook with a real per-window API;
//   the GPU DRAW path composites each window at its own opacity (and, for frosted
//   glass, its blur radius). Only the owning process may set its window's props.
static int64_t sys_brook_wm_set_window_properties(uint64_t wmId, uint64_t mask,
                                                   uint64_t opacity, uint64_t blurRadius,
                                                   uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    return brook::WmSetOwnedWindowProperties(proc, static_cast<uint32_t>(wmId),
                                             static_cast<uint32_t>(mask),
                                             static_cast<uint8_t>(opacity & 0xFF),
                                             static_cast<uint8_t>(blurRadius & 0xFF)) ? 0 : -EINVAL;
}

// ===========================================================================
// App-GPU syscalls (0xB10..): per-process virgl context for userspace GL-style
// rendering. The app gets its own context + resource ids, submits virgl command
// streams, and reads results back into its window VFB. Backed by GpuAppOps,
// which a 3D display driver registers. All gate on the caller owning the ctx
// (proc->gpuAppCtx). Numbers sit in the high 0xB00 block (clear of Linux/Windows).
// ===========================================================================

// 0xB10: GPU_CTX_CREATE() -> ctxId (>0), or <0. One context per process.
static int64_t sys_brook_gpu_ctx_create(uint64_t, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->CtxCreate) return -ENODEV;
    if (proc->gpuAppCtx > 0) return proc->gpuAppCtx;   // already have one
    int32_t ctx = ga->CtxCreate(proc->pid, {});
    if (ctx < 0) return -ENOMEM;
    proc->gpuAppCtx = ctx;
    return ctx;
}

// 0xB11: GPU_RESOURCE_CREATE(ctx, format, bind, w, h) -> host-global resId (>0)
// or <0. The returned id is used directly in the app's virgl streams.
static int64_t sys_brook_gpu_resource_create(uint64_t ctx, uint64_t format,
                                             uint64_t bind, uint64_t w, uint64_t h,
                                             uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->ResourceCreate3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return -EINVAL;
    // This native ABI only creates 2-D textures; pass PIPE_TEXTURE_2D (2) so the
    // driver picks the texture path (target 0 is reserved for PIPE_BUFFER).
    int32_t res = ga->ResourceCreate3D((int32_t)ctx, /*PIPE_TEXTURE_2D=*/2u,
                                       (uint32_t)format, (uint32_t)bind,
                                       (uint32_t)w, (uint32_t)h,
                                       /*depth=*/1u, /*arraySize=*/1u,
                                       /*lastLevel=*/0u, /*nrSamples=*/0u,
                                       /*flags=*/0u);
    return res < 0 ? -ENOMEM : res;
}

// 0xB12: GPU_ATTACH_WINDOW(ctx, resId, wmId) — back a resource with the caller's
// window VFB (kernel-mapped), so host-rendered pixels land straight in the
// window. Convenience for the common "render into my window" case.
static int64_t sys_brook_gpu_attach_window(uint64_t ctx, uint64_t resId,
                                           uint64_t wmId, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->ResourceAttachUser) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    brook::Window* win = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!win || !win->vfb || !win->vfbBytes) return -EINVAL;
    int32_t r = ga->ResourceAttachUser((int32_t)ctx, (int32_t)resId,
                                       reinterpret_cast<uint64_t>(win->vfb),
                                       static_cast<uint32_t>(win->vfbBytes));
    return r < 0 ? -EINVAL : 0;
}

// 0xB16: GPU_UPLOAD_BUFFER(ctx, srcPtr, bytes) -> host-global resId (>0) or <0.
// Creates a vertex-buffer resource holding a copy of the app's data (e.g.
// vertices) and pushes it to the host; the returned id is used in the app's
// virgl SET_VERTEX_BUFFERS command.
static int64_t sys_brook_gpu_upload_buffer(uint64_t ctx, uint64_t srcPtr,
                                           uint64_t bytes, uint64_t,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->BufferUpload) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (bytes == 0 || bytes > 16u * 1024 * 1024) return -EINVAL;
    if (!UserBufferReadable(srcPtr, bytes)) return -EFAULT;
    int32_t r = ga->BufferUpload((int32_t)ctx,
                                 reinterpret_cast<const void*>(srcPtr), (uint32_t)bytes);
    return r < 0 ? -EINVAL : r;
}

// 0xB13: GPU_SUBMIT(ctx, cmdPtr, nDwords) — submit a virgl command stream.
static int64_t sys_brook_gpu_submit(uint64_t ctx, uint64_t cmdPtr, uint64_t nDwords,
                                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->Submit3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (nDwords == 0 || nDwords > 8192) return -EINVAL;
    if (!UserBufferReadable(cmdPtr, nDwords * 4)) return -EFAULT;
    int32_t r = ga->Submit3D((int32_t)ctx, reinterpret_cast<const uint32_t*>(cmdPtr),
                             (uint32_t)nDwords);
    return r < 0 ? -EIO : 0;
}

// 0xB14: GPU_TRANSFER(ctx, resId, dir, w, h) — dir 0=to host, 1=from host.
static int64_t sys_brook_gpu_transfer(uint64_t ctx, uint64_t resId, uint64_t dir,
                                      uint64_t w, uint64_t h, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->Transfer3D) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    if (w == 0 || h == 0 || w > 8192 || h > 8192 || dir > 1) return -EINVAL;
    const brook::GpuTransfer3D transfer{
        0,0,0,(uint32_t)w,(uint32_t)h,1,0,0,(uint32_t)w*4,(uint32_t)(w*h*4)};
    int32_t r = ga->Transfer3D((int32_t)ctx, (int32_t)resId, (int)dir,
                               transfer);
    return r < 0 ? -EIO : 0;
}

// 0xB15: GPU_CTX_DESTROY(ctx) — release the process's GPU context.
static int64_t sys_brook_gpu_ctx_destroy(uint64_t ctx, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    const brook::GpuAppOps* ga = brook::GpuAppGet();
    if (!ga || !ga->CtxDestroy) return -ENODEV;
    if ((int32_t)ctx != proc->gpuAppCtx || proc->gpuAppCtx <= 0) return -EPERM;
    ga->CtxDestroy((int32_t)ctx);
    proc->gpuAppCtx = 0;
    return 0;
}

// 513: WM_BEGIN_MOVE(wmId)
//   Begin moving a waylandd-hosted window. Used to honour xdg_toplevel.move
//   for client-side-decorated windows, where the titlebar click lands in
//   client space and the client asks the compositor to start an interactive move.
static int64_t sys_brook_wm_begin_move(uint64_t wmId, uint64_t,
                                        uint64_t, uint64_t,
                                        uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    int idx = static_cast<int>(wmId) - 1;
    return brook::CompositorBeginWindowMove(idx) ? 0 : -EINVAL;
}

// 514: WM_MOVE_RELATIVE(wmId, parentWmId, relX, relY)
//   Position a waylandd-hosted popup relative to the parent window's client
//   area. xdg_popup positioners use parent-surface coordinates, while the
//   kernel WM stores outer window coordinates including optional chrome.
static int64_t sys_brook_wm_move_relative(uint64_t wmId, uint64_t parentWmId,
                                           uint64_t relX, uint64_t relY,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;
    if (parentWmId == 0 || parentWmId > 0xFFFFu) return -EINVAL;

    bool ok = brook::WmMoveWindowRelativeToParent(proc,
        static_cast<uint32_t>(wmId),
        static_cast<uint32_t>(parentWmId),
        static_cast<int32_t>(relX),
        static_cast<int32_t>(relY));
    return ok ? 0 : -EINVAL;
}

// 515: WM_SET_MAXIMIZED(wmId, enable)
static int64_t sys_brook_wm_set_maximized(uint64_t wmId, uint64_t enable,
                                           uint64_t, uint64_t,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    brook::WmSetMaximized(static_cast<int>(wmId) - 1, enable != 0);
    return 0;
}

// 516: WM_SET_MINIMIZED(wmId)
static int64_t sys_brook_wm_set_minimized(uint64_t wmId, uint64_t,
                                           uint64_t, uint64_t,
                                           uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    brook::WmMinimizeWindow(static_cast<int>(wmId) - 1);
    return 0;
}

// 517: WM_BEGIN_RESIZE(wmId, edges)
// Edge bits are Brook-local: 1=top, 2=bottom, 4=left, 8=right.
static int64_t sys_brook_wm_begin_resize(uint64_t wmId, uint64_t edges,
                                          uint64_t, uint64_t,
                                          uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    if (wmId == 0 || wmId > 0xFFFFu) return -EINVAL;
    if ((edges & ~0xFULL) != 0 || edges == 0) return -EINVAL;

    brook::Window* w = brook::WmFindWindowById(proc, static_cast<uint32_t>(wmId));
    if (!w) return -EINVAL;
    bool top = (edges & 1u) != 0;
    bool bottom = (edges & 2u) != 0;
    bool left = (edges & 4u) != 0;
    bool right = (edges & 8u) != 0;
    if ((top && bottom) || (left && right)) return -EINVAL;

    return brook::CompositorBeginWindowResize(static_cast<int>(wmId) - 1,
                                               left, right, top, bottom)
        ? 0 : -EINVAL;
}

// 518: WM_SET_CURSOR_VISIBLE(visible)
// Mechanism for userspace compositors to hide/show Brook's built-in cursor.
// This does not implement Wayland cursor surfaces or hotspot rendering.
static int64_t sys_brook_wm_set_cursor_visible(uint64_t visible, uint64_t,
                                                uint64_t, uint64_t,
                                                uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    return brook::CompositorSetCursorVisible(proc, visible != 0) ? 0 : -EPERM;
}

// 519: WM_SET_CURSOR_IMAGE(pixels_ptr, w, h, hotX, hotY)
// Upload a custom ARGB8888 cursor image (max 64×64). Pass w=0 to reset.
static int64_t sys_brook_wm_set_cursor_image(uint64_t pixelsAddr, uint64_t w,
                                              uint64_t h, uint64_t hotX,
                                              uint64_t hotY, uint64_t)
{
    if (w == 0) {
        brook::CompositorSetCursorImage(nullptr, 0, 0, 0, 0);
        return 0;
    }
    if (w > 64 || h > 64) return -EINVAL;
    if (h == 0) {
        brook::CompositorSetCursorImage(nullptr, 0, 0, 0, 0);
        return 0;
    }
    Process* proc = ProcessCurrent();
    if (!proc || !proc->addressSpace) return -ESRCH;
    KernelOperationGuard operation(proc);
    const uint64_t byteSize = w * h * 4;
    // Populate lazy pages before acquiring the AS lock. Validation alone is
    // not a lifetime pin: a sibling may unmap/protect the buffer immediately.
    if (!UserBufferReadable(pixelsAddr, byteSize)) return -EFAULT;
    auto* staged = static_cast<uint8_t*>(kmalloc(byteSize));
    if (!staged) return -ENOMEM;
    int64_t result = 0;
    {
        MappingTransaction lifetime(proc, proc->addressSpace);
        // Read-only resident copy: mutation serializes leaf retirement,
        // protection changes and file invalidation. No backing escapes this lock.
        KMutexLock(&proc->addressSpace->mutation);
        uint64_t offset = 0;
        while (!result && offset < byteSize) {
            const uint64_t address = pixelsAddr + offset;
            const auto* range = MappingRangeFind(proc->addressSpace->ranges, address);
            if (!range || !(range->protection & 1)) { result = -EFAULT; break; }
            const uint64_t leaf = VmmReadLeaf(proc->addressSpace->pageTable, VirtualAddress(address));
            if ((leaf & (VMM_PRESENT | VMM_USER)) != (VMM_PRESENT | VMM_USER)) { result = -EFAULT; break; }
            uint64_t chunk = 4096 - (address & 4095);
            if (chunk > byteSize - offset) chunk = byteSize - offset;
            if (chunk > range->end - address) chunk = range->end - address;
            memcpy(staged + offset, reinterpret_cast<const void*>(address), chunk);
            offset += chunk;
        }
        KMutexUnlock(&proc->addressSpace->mutation);
    }
    // Cursor publication reads kernel memory only, after releasing the AS lock.
    if (!result && !brook::CompositorSetCursorImage(reinterpret_cast<const uint32_t*>(staged),
            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            static_cast<int32_t>(hotX), static_cast<int32_t>(hotY))) result = -EINVAL;
    kfree(staged);
    return result;
}

// 520: WM_GET_SCREEN_INFO(out_ptr) — return screen width and height
// Writes two uint32_t values [width, height] to the user-supplied buffer.
static int64_t sys_brook_wm_get_screen_info(uint64_t outAddr, uint64_t, uint64_t,
                                             uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(outAddr, 8)) return -EFAULT;
    uint32_t w = 0, h = 0;
    brook::CompositorGetPhysDims(&w, &h);
    uint32_t* out = reinterpret_cast<uint32_t*>(outAddr);
    out[0] = w;
    out[1] = h;
    return 0;
}

// ---------------------------------------------------------------------------
// 521: WM_SCANOUT_MAP(out_ptr) — map compositor backbuffer into caller's space.
// Writes a ScanoutMapResult struct to out_ptr.
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_scanout_map(uint64_t outAddr, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(outAddr, sizeof(brook::ScanoutMapResult)))
        return -EFAULT;

    Process* proc = ProcessCurrent();
    auto* out = reinterpret_cast<brook::ScanoutMapResult*>(outAddr);
    uint64_t addr = brook::CompositorScanoutMap(proc, out);
    return addr ? 0 : -ENOMEM;
}

// ---------------------------------------------------------------------------
// 522: WM_SCANOUT_FLIP(minY, maxY) — copy dirty rows to MMIO framebuffer.
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_scanout_flip(uint64_t minY, uint64_t maxY, uint64_t,
                                          uint64_t, uint64_t, uint64_t)
{
    return brook::CompositorScanoutFlip(static_cast<uint32_t>(minY),
                                        static_cast<uint32_t>(maxY));
}

// ---------------------------------------------------------------------------
// 523: WM_GET_WINDOWS(out_ptr, max_count) — query window state for usermode compositor
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_get_windows(uint64_t outAddr, uint64_t maxCount, uint64_t,
                                         uint64_t, uint64_t, uint64_t)
{
    if (maxCount == 0) return 0;
    uint64_t bufSize = maxCount * sizeof(brook::WmWindowDesc);
    if (!UserBufferWritable(outAddr, bufSize)) return -EFAULT;
    Process* proc = ProcessCurrent();
    auto* out = reinterpret_cast<brook::WmWindowDesc*>(outAddr);
    return static_cast<int64_t>(brook::CompositorGetWindows(proc, out, static_cast<uint32_t>(maxCount)));
}

// ---------------------------------------------------------------------------
// 524: WM_MAP_WINDOW_VFB(wmId) — map a window's VFB into compositor process
// ---------------------------------------------------------------------------
static int64_t sys_brook_wm_map_window_vfb(uint64_t wmId, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    uint64_t addr = brook::CompositorMapWindowVfb(proc, static_cast<uint32_t>(wmId));
    return addr ? static_cast<int64_t>(addr) : -ENOENT;
}

// ---------------------------------------------------------------------------
// sys_not_implemented
// ---------------------------------------------------------------------------

static int64_t sys_not_implemented(uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t)
{
    // Rate-limit: log first 8 per-syscall, then every 256th, to avoid
    // serial lock contention when many processes hit unimplemented syscalls.
    static volatile uint32_t s_unimplCount = 0;
    uint32_t n = __atomic_fetch_add(&s_unimplCount, 1, __ATOMIC_RELAXED);

    Process* proc = ProcessCurrent();
    uint64_t syscallNum = 0;
    __asm__ volatile("mov %%gs:120, %0" : "=r"(syscallNum));

    if (n < 8 || (n & 0xFF) == 0)
        SerialPrintf("UNIMPL: syscall %lu from pid %u ('%s') [#%u]\n",
                     syscallNum, proc ? proc->pid : 0, proc ? proc->name : "?", n);
    return -ENOSYS;
}

// Silent ENOSYS: for syscalls we deliberately don't implement but where
// the userland-side fallback is well-trodden (e.g. inotify -> polling).
// Avoids noisy UNIMPL spam during normal operation.
static int64_t sys_enosys_quiet(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t)
{
    return -ENOSYS;
}

// Lives in its own page (.syscall_table) so we can mark it read-only
// after init. See VmmKernelMarkReadOnly + linker.ld.
static SyscallFn g_syscallTable[SYSCALL_MAX]
    __attribute__((section(".syscall_table"), aligned(4096)));

extern "C" uint8_t __syscall_table_start[];
extern "C" uint8_t __syscall_table_end[];

void SyscallTableInit()
{
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        g_syscallTable[i] = sys_not_implemented;

    g_syscallTable[SYS_READ]            = sys_read;
    g_syscallTable[SYS_WRITE]           = sys_write;
    g_syscallTable[SYS_OPEN]            = sys_open;
    g_syscallTable[SYS_CREAT]           = sys_creat;
    g_syscallTable[SYS_CLOSE]           = sys_close;
    g_syscallTable[SYS_STAT]            = sys_stat;
    g_syscallTable[SYS_FSTAT]           = sys_fstat;
    g_syscallTable[SYS_LSTAT]           = sys_lstat;
    g_syscallTable[SYS_LSEEK]           = sys_lseek;
    g_syscallTable[SYS_MMAP]            = sys_mmap;
    g_syscallTable[SYS_MPROTECT]        = sys_mprotect;
    g_syscallTable[SYS_MUNMAP]          = sys_munmap;
    g_syscallTable[SYS_MREMAP]          = sys_mremap;
    g_syscallTable[SYS_MSYNC]           = sys_msync;
    g_syscallTable[SYS_BRK]             = sys_brk;
    g_syscallTable[SYS_RT_SIGACTION]    = sys_rt_sigaction;
    g_syscallTable[SYS_RT_SIGPROCMASK]  = sys_rt_sigprocmask;
    g_syscallTable[SYS_IOCTL]           = sys_ioctl;
    g_syscallTable[SYS_READV]           = sys_readv;
    g_syscallTable[SYS_WRITEV]          = sys_writev;
    g_syscallTable[SYS_ACCESS]          = sys_access;
    g_syscallTable[SYS_SCHED_YIELD]     = sys_sched_yield;
    g_syscallTable[324]                = sys_membarrier;
    g_syscallTable[SYS_NANOSLEEP]       = sys_nanosleep;
    g_syscallTable[SYS_GETPID]          = sys_getpid;
    g_syscallTable[SYS_PIPE]            = sys_pipe;
    g_syscallTable[SYS_DUP]             = sys_dup;
    g_syscallTable[SYS_DUP2]            = sys_dup2;
    g_syscallTable[SYS_CLONE]           = sys_clone;
    g_syscallTable[SYS_FORK]            = sys_fork;
    g_syscallTable[SYS_VFORK]           = sys_vfork;
    g_syscallTable[435]                 = sys_clone3;
    g_syscallTable[SYS_EXECVE]          = sys_execve;
    g_syscallTable[SYS_EXIT]            = sys_exit;
    g_syscallTable[SYS_WAIT4]           = sys_wait4;
    g_syscallTable[SYS_WAITID]          = sys_waitid;
    g_syscallTable[SYS_UNAME]           = sys_uname;
    g_syscallTable[SYS_FCNTL]           = sys_fcntl;
    g_syscallTable[SYS_FSYNC]           = sys_fsync;
    g_syscallTable[SYS_FDATASYNC]       = sys_fsync;
    g_syscallTable[SYS_SYNC]            = sys_sync;
    g_syscallTable[SYS_PIVOT_ROOT]      = sys_pivot_root;
    g_syscallTable[SYS_MOUNT]           = sys_mount;
    g_syscallTable[SYS_UMOUNT2]         = sys_umount2;
    g_syscallTable[SYS_GETCWD]          = sys_getcwd;
    g_syscallTable[SYS_GETTIMEOFDAY]    = sys_gettimeofday;
    g_syscallTable[SYS_TIME]             = sys_time;
    g_syscallTable[SYS_GETUID]          = sys_getuid;
    g_syscallTable[SYS_GETGID]          = sys_getgid;
    g_syscallTable[SYS_SETUID]          = sys_setuid;
    g_syscallTable[SYS_SETGID]          = sys_setgid;
    g_syscallTable[113] = sys_setreuid;
    g_syscallTable[114] = sys_setregid;
    g_syscallTable[117] = sys_setresuid;
    g_syscallTable[119] = sys_setresgid;
    g_syscallTable[SYS_CAPGET]          = sys_capget;
    g_syscallTable[SYS_CAPSET]          = sys_capset;
    // inotify: deliberately unimplemented. Glibc/GLib fall back to
    // polling/manual checks when inotify_init fails. Returning ENOSYS
    // quietly avoids UNIMPL serial spam from GIMP / file pickers.
    g_syscallTable[253]                  = sys_enosys_quiet; // inotify_init
    g_syscallTable[254]                  = sys_enosys_quiet; // inotify_add_watch
    g_syscallTable[255]                  = sys_enosys_quiet; // inotify_rm_watch
    g_syscallTable[294]                  = sys_enosys_quiet; // inotify_init1
    g_syscallTable[SYS_GETEUID]         = sys_geteuid;
    g_syscallTable[SYS_GETEGID]         = sys_getegid;
    g_syscallTable[SYS_GETPPID]         = sys_getppid;
    g_syscallTable[SYS_GETGROUPS]       = sys_getgroups;
    g_syscallTable[SYS_SETGROUPS]       = sys_setgroups;
    g_syscallTable[SYS_ARCH_PRCTL]      = sys_arch_prctl;
    g_syscallTable[SYS_GETDENTS64]      = sys_getdents64;
    g_syscallTable[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    g_syscallTable[SYS_CLOCK_GETTIME]   = sys_clock_gettime;
    g_syscallTable[SYS_CLOCK_GETRES]    = sys_clock_getres;
    g_syscallTable[SYS_CLOCK_NANOSLEEP] = sys_clock_nanosleep;
    g_syscallTable[SYS_EXIT_GROUP]      = sys_exit_group;
    g_syscallTable[SYS_OPENAT]          = sys_openat;
    g_syscallTable[SYS_NEWFSTATAT]      = sys_newfstatat;
    g_syscallTable[SYS_PRLIMIT64]       = sys_prlimit64;
    g_syscallTable[SYS_GETRANDOM]       = sys_getrandom;

    // New syscalls
    g_syscallTable[SYS_POLL]            = sys_poll;
    g_syscallTable[SYS_RT_SIGRETURN]    = sys_rt_sigreturn;
    g_syscallTable[SYS_SELECT]          = sys_select;
    g_syscallTable[SYS_PSELECT6]        = sys_pselect6;
    g_syscallTable[SYS_SENDFILE]        = sys_sendfile;
    g_syscallTable[SYS_KILL]            = sys_kill;
    g_syscallTable[SYS_CHDIR]           = sys_chdir;
    g_syscallTable[SYS_FCHDIR]          = sys_fchdir;
    g_syscallTable[SYS_RENAME]          = sys_rename;
    g_syscallTable[SYS_MKNOD] = sys_mknod;
    g_syscallTable[SYS_MKNODAT] = sys_mknodat;
    g_syscallTable[SYS_MKDIR]           = sys_mkdir;
    g_syscallTable[SYS_RMDIR]           = sys_rmdir;
    g_syscallTable[SYS_UNLINK]          = sys_unlink;
    g_syscallTable[SYS_SYMLINK]         = sys_symlink;
    g_syscallTable[SYS_READLINK]        = sys_readlink;
    g_syscallTable[SYS_UMASK]           = sys_umask;
    g_syscallTable[SYS_GETRLIMIT]       = sys_getrlimit;
    g_syscallTable[SYS_GETRUSAGE]       = sys_getrusage;
    g_syscallTable[SYS_SYSINFO]         = sys_sysinfo;
    g_syscallTable[SYS_TIMES]           = sys_times;
    g_syscallTable[SYS_SETPGID]         = sys_setpgid;
    g_syscallTable[SYS_GETPGRP]         = sys_getpgrp;
    g_syscallTable[SYS_SETSID]          = sys_setsid;
    g_syscallTable[SYS_GETPGID]         = sys_getpgid;
    g_syscallTable[SYS_GETSID]          = sys_getsid;
    g_syscallTable[SYS_SIGALTSTACK]     = sys_sigaltstack;
    g_syscallTable[SYS_ALARM]           = sys_alarm;
    g_syscallTable[SYS_PAUSE]           = sys_pause;
    g_syscallTable[SYS_RT_SIGPENDING] = sys_rt_sigpending;
    g_syscallTable[SYS_RT_SIGTIMEDWAIT] = sys_rt_sigtimedwait;
    g_syscallTable[SYS_RT_SIGSUSPEND]   = sys_rt_sigsuspend;
    g_syscallTable[SYS_STATFS]          = sys_statfs;
    g_syscallTable[SYS_FSTATFS]         = sys_fstatfs;
    g_syscallTable[SYS_GETTID]          = sys_gettid;
    g_syscallTable[SYS_TKILL]           = sys_tkill;
    g_syscallTable[SYS_TGKILL]          = sys_tgkill;
    g_syscallTable[SYS_READLINKAT]      = sys_readlinkat;
    g_syscallTable[SYS_SYMLINKAT]       = sys_symlinkat;
    g_syscallTable[SYS_PPOLL]           = sys_ppoll;
    g_syscallTable[SYS_PIPE2]           = sys_pipe2;
    g_syscallTable[SYS_EVENTFD2]        = sys_eventfd2;
    g_syscallTable[SYS_DUP3]            = sys_dup3;

    // Bash / POSIX compatibility
    g_syscallTable[SYS_PREAD64]         = sys_pread64;
    g_syscallTable[SYS_GETRESUID]       = sys_getresuid;
    g_syscallTable[SYS_GETRESGID]       = sys_getresgid;
    g_syscallTable[SYS_PRCTL]           = sys_prctl;
    g_syscallTable[SYS_FUTEX]           = sys_futex;
    g_syscallTable[SYS_SIGNALFD]        = sys_signalfd;
    g_syscallTable[SYS_SIGNALFD4]       = sys_signalfd4;
    g_syscallTable[SYS_SEMGET]          = sys_semget;
    g_syscallTable[SYS_SEMOP]           = sys_semop;
    g_syscallTable[SYS_SEMTIMEDOP]      = sys_semtimedop;
    g_syscallTable[SYS_SEMCTL]          = sys_semctl;
    g_syscallTable[SYS_SET_ROBUST_LIST] = sys_set_robust_list;
    g_syscallTable[SYS_GET_ROBUST_LIST] = sys_get_robust_list;
    g_syscallTable[SYS_FACCESSAT]       = sys_faccessat;
    g_syscallTable[SYS_RSEQ]            = sys_rseq;
    g_syscallTable[SYS_CLOSE_RANGE]     = sys_close_range;
    g_syscallTable[SYS_FACCESSAT2]      = sys_faccessat2;

    // Nix package manager syscalls
    g_syscallTable[SYS_FLOCK]           = sys_flock;
    g_syscallTable[SYS_TRUNCATE]        = sys_truncate;
    g_syscallTable[SYS_FTRUNCATE]       = sys_ftruncate;
    g_syscallTable[SYS_LINK]            = sys_link;
    g_syscallTable[SYS_CHMOD]           = sys_chmod;
    g_syscallTable[SYS_FCHMOD]          = sys_fchmod;
    g_syscallTable[SYS_CHOWN]           = sys_chown;
    g_syscallTable[SYS_FCHOWN]          = sys_fchown;
    g_syscallTable[SYS_LCHOWN]          = sys_lchown;
    g_syscallTable[SYS_SCHED_SETPARAM]  = sys_sched_setparam;
    g_syscallTable[SYS_SCHED_GETPARAM]  = sys_sched_getparam;
    g_syscallTable[SYS_SCHED_SETAFFINITY] = sys_sched_setaffinity;
    g_syscallTable[SYS_SCHED_GETAFFINITY] = sys_sched_getaffinity;
    g_syscallTable[SYS_SCHED_SETSCHEDULER] = sys_sched_setscheduler;
    g_syscallTable[SYS_SCHED_GETSCHEDULER] = sys_sched_getscheduler;
    g_syscallTable[SYS_SCHED_GET_PRIORITY_MAX] = sys_sched_get_priority_max;
    g_syscallTable[SYS_SCHED_GET_PRIORITY_MIN] = sys_sched_get_priority_min;
    g_syscallTable[SYS_MKDIRAT]         = sys_mkdirat;
    g_syscallTable[SYS_FCHOWNAT]        = sys_fchownat;
    g_syscallTable[SYS_UNLINKAT]        = sys_unlinkat;
    g_syscallTable[SYS_RENAMEAT]        = sys_renameat;
    g_syscallTable[SYS_LINKAT]          = sys_linkat;
    g_syscallTable[SYS_FCHMODAT]        = sys_fchmodat;
    g_syscallTable[SYS_UTIMENSAT]       = sys_utimensat;
    g_syscallTable[SYS_FALLOCATE]       = sys_fallocate;
    g_syscallTable[SYS_RENAMEAT2]       = sys_renameat2;
    g_syscallTable[SYS_STATX]           = sys_statx;
    g_syscallTable[SYS_MINCORE]         = sys_mincore;
    g_syscallTable[SYS_MADVISE]         = sys_madvise;
    g_syscallTable[SYS_SHMGET]          = sys_enosys_quiet;
    g_syscallTable[SYS_GETPRIORITY]     = sys_getpriority;
    g_syscallTable[SYS_SETPRIORITY]     = sys_setpriority;
    g_syscallTable[SYS_PIDFD_OPEN]      = sys_enosys_quiet;
    g_syscallTable[SYS_FADVISE64]       = sys_fadvise64;
    g_syscallTable[SYS_MBIND]           = sys_mbind;
    g_syscallTable[SYS_SET_MEMPOLICY]   = sys_enosys_quiet;
    g_syscallTable[SYS_GET_MEMPOLICY]   = sys_enosys_quiet;
    g_syscallTable[SYS_PWRITE64]        = sys_pwrite64;

    // Socket syscalls
    g_syscallTable[SYS_SOCKET]          = sys_socket;
    g_syscallTable[SYS_CONNECT]         = sys_connect;
    g_syscallTable[SYS_ACCEPT]          = sys_accept;
    g_syscallTable[SYS_ACCEPT4]         = sys_accept4;
    g_syscallTable[SYS_SENDTO]          = sys_sendto;
    g_syscallTable[SYS_RECVFROM]        = sys_recvfrom;
    g_syscallTable[SYS_SENDMSG]         = sys_sendmsg;
    g_syscallTable[SYS_RECVMSG]         = sys_recvmsg;
    g_syscallTable[SYS_SENDMMSG]        = sys_sendmmsg;
    g_syscallTable[SYS_RECVMMSG]        = sys_recvmmsg;
    g_syscallTable[SYS_SHUTDOWN]        = sys_shutdown;
    g_syscallTable[SYS_BIND]            = sys_bind;
    g_syscallTable[SYS_LISTEN]          = sys_listen;
    g_syscallTable[SYS_GETSOCKNAME]     = sys_getsockname;
    g_syscallTable[SYS_GETPEERNAME]     = sys_getpeername;
    g_syscallTable[SYS_SOCKETPAIR]      = sys_socketpair;
    g_syscallTable[SYS_SETSOCKOPT]      = sys_setsockopt;
    g_syscallTable[SYS_GETSOCKOPT]      = sys_getsockopt;

    // Wayland prerequisites
    g_syscallTable[SYS_EPOLL_CREATE]    = sys_epoll_create;
    g_syscallTable[SYS_EPOLL_CREATE1]   = sys_epoll_create1;
    g_syscallTable[SYS_EPOLL_CTL]       = sys_epoll_ctl;
    g_syscallTable[SYS_EPOLL_WAIT]      = sys_epoll_wait;
    g_syscallTable[SYS_EPOLL_PWAIT]     = sys_epoll_pwait;
    g_syscallTable[SYS_TIMER_CREATE] = sys_timer_create;
    g_syscallTable[SYS_TIMER_SETTIME] = sys_timer_settime;
    g_syscallTable[SYS_TIMER_GETTIME] = sys_timer_gettime;
    g_syscallTable[SYS_TIMER_GETOVERRUN] = sys_timer_getoverrun;
    g_syscallTable[SYS_TIMER_DELETE] = sys_timer_delete;
    g_syscallTable[SYS_TIMERFD_CREATE]  = sys_timerfd_create;
    g_syscallTable[SYS_TIMERFD_SETTIME] = sys_timerfd_settime;
    g_syscallTable[SYS_TIMERFD_GETTIME] = sys_timerfd_gettime;
    g_syscallTable[SYS_MEMFD_CREATE]    = sys_memfd_create;

    // Brook-specific syscalls (500+). 500 = profiler control.
    g_syscallTable[500]                  = sys_brook_profile;
    g_syscallTable[502]                  = sys_brook_set_crash_entry;
    g_syscallTable[503]                  = sys_brook_crash_complete;
    g_syscallTable[504]                  = sys_brook_input_pop;
    g_syscallTable[505]                  = sys_brook_input_grab;
    g_syscallTable[506]                  = sys_brook_wm_create_window;
    g_syscallTable[507]                  = sys_brook_wm_destroy_window;
    g_syscallTable[508]                  = sys_brook_wm_signal_dirty;
    g_syscallTable[509]                  = sys_brook_wm_set_title;
    g_syscallTable[510]                  = sys_brook_wm_pop_input;
    g_syscallTable[511]                  = sys_brook_wm_resize_vfb;
    g_syscallTable[512]                  = sys_brook_wm_set_decoration_mode;
    g_syscallTable[513]                  = sys_brook_wm_begin_move;
    g_syscallTable[514]                  = sys_brook_wm_move_relative;
    g_syscallTable[515]                  = sys_brook_wm_set_maximized;
    g_syscallTable[516]                  = sys_brook_wm_set_minimized;
    g_syscallTable[517]                  = sys_brook_wm_begin_resize;
    g_syscallTable[518]                  = sys_brook_wm_set_cursor_visible;
    g_syscallTable[519]                  = sys_brook_wm_set_cursor_image;
    g_syscallTable[520]                  = sys_brook_wm_get_screen_info;
    g_syscallTable[521]                  = sys_brook_wm_scanout_map;
    g_syscallTable[522]                  = sys_brook_wm_scanout_flip;
    g_syscallTable[523]                  = sys_brook_wm_get_windows;
    g_syscallTable[524]                  = sys_brook_wm_map_window_vfb;
    g_syscallTable[525]                  = sys_brook_wm_present_gres;
    // Brook windowing extensions live in a high 0xB00 block, clear of the Linux
    // and Windows syscall ranges.
    g_syscallTable[0xB00]                = sys_brook_wm_set_window_properties; // WM_SET_WINDOW_PROPERTIES
    g_syscallTable[0xB01]                = sys_brook_wm_set_app_id;
    g_syscallTable[0xB05]                = sys_brook_wm_set_client_socket;
    g_syscallTable[0xB06]                = sys_brook_scene_create;
    g_syscallTable[0xB07]                = sys_brook_scene_ctl;
    g_syscallTable[0xB08]                = sys_brook_wm_window_state;
    g_syscallTable[0xB09]                = sys_brook_wm_place_window;
    g_syscallTable[0xB0A]                = sys_brook_wm_pointer_capture;
    g_syscallTable[BROOK_SYS_GPU_OUTPUT_ALLOC] = sys_brook_gpu_output_alloc;
    g_syscallTable[BROOK_SYS_GPU_BUFFER_TRY_CPU_READ] = sys_brook_gpu_buffer_try_cpu_read;
    g_syscallTable[BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE] = sys_brook_gpu_buffer_snapshot_fence;
    g_syscallTable[BROOK_SYS_GPU_WINDOW_VALIDATE] = sys_brook_gpu_window_validate;
    g_syscallTable[BROOK_SYS_GPU_WINDOW_PEER_RECOVERY] = sys_brook_gpu_window_peer_recovery;
    g_syscallTable[BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT] = sys_brook_wm_publish_source_commit;
    g_syscallTable[BROOK_SYS_WM_QUERY_SOURCE_COMMIT] = sys_brook_wm_query_source_commit;
    g_syscallTable[BROOK_SYS_WM_BEGIN_SOURCE_WRITE] = sys_brook_wm_begin_source_write;
    g_syscallTable[BROOK_SYS_GPU_HANDOFF_CTL] = sys_brook_gpu_handoff_ctl;
    g_syscallTable[BROOK_SYS_DESKTOP_LAYERS] = sys_brook_desktop_layers;
    g_syscallTable[0xB02]                = sys_brook_wm_theme;
    g_syscallTable[0xB03]                = sys_brook_wm_refresh_apps;
    g_syscallTable[0xB04]                = sys_brook_wm_scale;
    g_syscallTable[BROOK_SYS_DEBUG_FRAME] = sys_brook_debug_frame;
    g_syscallTable[BROOK_SYS_DESKTOP_OVERLAY] = sys_brook_desktop_overlay;
    g_syscallTable[BROOK_SYS_AHCI_EXPORT] = sys_brook_ahci_export;
    g_syscallTable[BROOK_SYS_SYSTEM_MESSAGE] = sys_brook_system_message;
    g_syscallTable[0xB40]                = sys_brook_pageable_memory;
    g_syscallTable[BROOK_SYS_WM_APPEARANCE] = sys_brook_wm_appearance;
    g_syscallTable[BROOK_SYS_WM_TASKBAR_METRICS] = sys_brook_wm_taskbar_metrics;
    g_syscallTable[BROOK_SYS_POINTER_SPEED] = sys_brook_pointer_speed;
    g_syscallTable[BROOK_SYS_AUDIO_CONTROL] = sys_brook_audio_control;
#ifdef BROOK_MAPPING_TEST_HOOKS
    g_syscallTable[0xB30]                = MappingFaultTestControl;
#endif
#ifdef BROOK_REGISTER_TEST_HOOKS
    g_syscallTable[0xB31]                = RegisterTestControl;
#endif
    // App-GPU (per-process virgl) syscalls for userspace GL rendering.
    g_syscallTable[0xB10]                = sys_brook_gpu_ctx_create;       // GPU_CTX_CREATE
    g_syscallTable[0xB11]                = sys_brook_gpu_resource_create;  // GPU_RESOURCE_CREATE
    g_syscallTable[0xB12]                = sys_brook_gpu_attach_window;    // GPU_ATTACH_WINDOW
    g_syscallTable[0xB16]                = sys_brook_gpu_upload_buffer;    // GPU_UPLOAD_BUFFER
    g_syscallTable[0xB13]                = sys_brook_gpu_submit;           // GPU_SUBMIT
    g_syscallTable[0xB14]                = sys_brook_gpu_transfer;         // GPU_TRANSFER
    g_syscallTable[0xB15]                = sys_brook_gpu_ctx_destroy;      // GPU_CTX_DESTROY

    uint32_t count = 0;
    for (uint64_t i = 0; i < SYSCALL_MAX; ++i)
        if (g_syscallTable[i] != sys_not_implemented) ++count;

    SerialPrintf("SYSCALL: table initialised (%u entries, %u implemented)\n",
                 static_cast<unsigned>(SYSCALL_MAX), count);

    // Lock the dispatch table read-only. Any subsequent write attempt
    // (corruption, stray pointer) will #PF immediately on the offending
    // RIP rather than silently leaving a null entry to be discovered
    // later by an indirect call. The table is page-aligned and may span
    // multiple pages as Brook-specific syscall ranges grow.
    uint64_t lockStart = reinterpret_cast<uint64_t>(__syscall_table_start);
    uint64_t lockEnd   = reinterpret_cast<uint64_t>(__syscall_table_end);
    if (VmmKernelMarkReadOnly(VirtualAddress(lockStart), lockEnd - lockStart))
        SerialPrintf("SYSCALL: dispatch table locked RO at 0x%lx (%lu bytes)\n",
                     lockStart, lockEnd - lockStart);
    else
        SerialPrintf("SYSCALL: WARNING failed to lock table RO\n");
}

SyscallFn* SyscallGetTable()
{
    return g_syscallTable;
}

uint64_t SyscallGetTableAddress()
{
    return reinterpret_cast<uint64_t>(g_syscallTable);
}

// ---------------------------------------------------------------------------
// Entry point address (for LSTAR MSR)
// ---------------------------------------------------------------------------

uint64_t SyscallGetEntryPoint()
{
    return reinterpret_cast<uint64_t>(&BrookSyscallDispatcher);
}

// ---------------------------------------------------------------------------
// Strace — syscall tracing facility
// ---------------------------------------------------------------------------

// Category bitmask for strace filtering
enum StraceCategory : uint32_t {
    SC_NET    = 1 << 0,  // socket, connect, sendto, recvfrom, etc.
    SC_FILE   = 1 << 1,  // open, read, write, close, stat, etc.
    SC_MEM    = 1 << 2,  // mmap, munmap, mprotect, brk
    SC_PROC   = 1 << 3,  // fork, clone, execve, exit, wait
    SC_SIG    = 1 << 4,  // rt_sigaction, rt_sigprocmask, kill, etc.
    SC_IO     = 1 << 5,  // ioctl, poll, pselect, ppoll, epoll
    SC_OTHER  = 1 << 6,  // everything else
};

static uint32_t SyscallCategory(uint64_t num)
{
    switch (num) {
    // Network
    case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48:
    case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 299: case 307:
        return SC_NET;  // socket..shutdown, recvmmsg, sendmmsg
    // File
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 8:
    case 17: case 18: case 19: case 20: case 21:
    case 32: case 33: case 72: case 73: case 76: case 77:
    case 79: case 80: case 82: case 83: case 84: case 85: case 86: case 87: case 88:
    case 89: case 90: case 91: case 92: case 93: case 94:
    case 217: case 257: case 258: case 260: case 262: case 263: case 264:
    case 265: case 266: case 267: case 268: case 280: case 285:
    case 292: case 293: case 316: case 332: case 439:
        return SC_FILE;
    // Memory
    case 9: case 10: case 11: case 12: case 25: case 26: case 27: case 28:
        return SC_MEM;  // mmap..mremap, madvise
    // Process
    case 56: case 57: case 58: case 59: case 60: case 61: case 62:
    case 231: case 234: case 247: case 435:
        return SC_PROC; // clone..wait4, exit_group, tgkill, clone3
    // Signal
    case 13: case 14: case 15: case 34: case 127: case 128: case 129: case 130: case 131:
    case 200:
        return SC_SIG;  // rt_sigaction..rt_sigreturn, kill, tkill
    // I/O control
    case 7: case 16: case 23: case 270: case 271:
    case 213: case 214: case 232: case 233:
    case 290:
        return SC_IO;   // poll, ioctl, select, pselect6, ppoll, epoll_*
    default:
        return SC_OTHER;
    }
}

static const char* SyscallName(uint64_t num)
{
    switch (num) {
    case 0: return "read";        case 1: return "write";
    case 2: return "open";        case 3: return "close";
    case 4: return "stat";        case 5: return "fstat";
    case 6: return "lstat";       case 7: return "poll";
    case 8: return "lseek";       case 9: return "mmap";
    case 10: return "mprotect";   case 11: return "munmap";
    case 12: return "brk";        case 13: return "rt_sigaction";
    case 14: return "rt_sigprocmask"; case 15: return "rt_sigreturn";
    case 16: return "ioctl";      case 17: return "pread64";
    case 19: return "readv";      case 20: return "writev";
    case 21: return "access";     case 22: return "pipe";
    case 23: return "select";     case 24: return "sched_yield";
    case 25: return "mremap";
    case 26: return "msync";
    case 32: return "dup";        case 33: return "dup2";
    case 35: return "nanosleep";  case 39: return "getpid";
    case 40: return "sendfile";   case 41: return "socket";
    case 42: return "connect";    case 43: return "accept";
    case 44: return "sendto";     case 45: return "recvfrom";
    case 46: return "sendmsg";    case 47: return "recvmsg";
    case 48: return "shutdown";   case 49: return "bind";
    case 50: return "listen";     case 51: return "getsockname";
    case 52: return "getpeername"; case 53: return "socketpair";
    case 54: return "setsockopt"; case 55: return "getsockopt";
    case 56: return "clone";
    case 57: return "fork";       case 58: return "vfork";
    case 59: return "execve";     case 60: return "exit";
    case 247: return "waitid";
    case 61: return "wait4";      case 62: return "kill";
    case 63: return "uname";      case 72: return "fcntl";
    case 74: return "fsync";      case 75: return "fdatasync";
    case 79: return "getcwd";     case 80: return "chdir";
    case 81: return "fchdir";     case 82: return "rename";
    case 83: return "mkdir";      case 84: return "rmdir";      case 87: return "unlink";
    case 85: return "creat";       case 88: return "symlink";    case 89: return "readlink";   case 95: return "umask";
    case 96: return "gettimeofday"; case 97: return "getrlimit";
    case 98: return "getrusage";  case 99: return "sysinfo";
    case 102: return "getuid";    case 104: return "getgid";
    case 105: return "setuid";    case 106: return "setgid";
    case 107: return "geteuid";   case 108: return "getegid";
    case 109: return "setpgid";   case 110: return "getppid";
    case 111: return "getpgrp";   case 112: return "setsid";
    case 115: return "getgroups"; case 116: return "setgroups";
    case 113: return "setreuid"; case 114: return "setregid";
    case 117: return "setresuid"; case 118: return "getresuid";
    case 119: return "setresgid"; case 120: return "getresgid";
    case 121: return "getpgid";   case 124: return "getsid";
    case 222: return "timer_create"; case 223: return "timer_settime";
    case 224: return "timer_gettime"; case 225: return "timer_getoverrun";
    case 226: return "timer_delete";
    case 127: return "rt_sigpending";
    case 128: return "rt_sigtimedwait"; case 131: return "sigaltstack"; case 130: return "rt_sigsuspend";
    case 134: return "statfs";
    case 34: return "pause"; case 37: return "alarm";
    case 135: return "fstatfs";   case 157: return "prctl";
    case 158: return "arch_prctl"; case 186: return "gettid";
    case 200: return "tkill";     case 201: return "time";       case 217: return "getdents64";
    case 218: return "set_tid_address"; case 228: return "clock_gettime";
    case 230: return "clock_nanosleep"; case 231: return "exit_group";
    case 234: return "tgkill";    case 257: return "openat";
    case 262: return "newfstatat"; case 266: return "symlinkat"; case 267: return "readlinkat";
    case 270: return "pselect6";  case 271: return "ppoll";     case 273: return "set_robust_list"; case 274: return "get_robust_list";
    case 292: return "dup3";
    case 290: return "eventfd2";  case 293: return "pipe2";     case 302: return "prlimit64";
    case 299: return "recvmmsg";  case 307: return "sendmmsg";
    case 318: return "getrandom"; case 334: return "rseq";
    case 439: return "faccessat2";
    case 73: return "flock";      case 76: return "truncate";
    case 77: return "ftruncate";  case 86: return "link";
    case 90: return "chmod";      case 91: return "fchmod";
    case 92: return "chown";      case 93: return "fchown";
    case 94: return "lchown";
    case 142: return "sched_setparam"; case 143: return "sched_getparam";
    case 144: return "sched_setscheduler"; case 145: return "sched_getscheduler";
    case 146: return "sched_get_priority_max"; case 147: return "sched_get_priority_min";
    case 203: return "sched_setaffinity"; case 204: return "sched_getaffinity";
    case 258: return "mkdirat";   case 260: return "fchownat";
    case 263: return "unlinkat";  case 264: return "renameat";
    case 265: return "linkat";    case 268: return "fchmodat";
    case 280: return "utimensat"; case 285: return "fallocate";
    case 316: return "renameat2"; case 332: return "statx";
    case 435: return "clone3";
    case 18: return "pwrite64"; case 27: return "mincore";
    case 28: return "madvise";
    default: return nullptr;
    }
}

static int64_t SyscallDispatchTraced(uint64_t num, uint64_t a0, uint64_t a1,
                                      uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    Process* proc = ProcessCurrent();
    const char* name = SyscallName(num);

    // Rate-limit: suppress repeated identical error syscalls to prevent
    // stack/serial flood (e.g. writes to a closed fd in a tight loop).
    static uint64_t s_lastNum   = ~0ULL;
    static uint64_t s_lastA0    = 0;
    static int64_t  s_lastRet   = 0;
    static uint32_t s_suppressed = 0;
    static constexpr uint32_t SUPPRESS_THRESHOLD = 3;

    SyscallFn fn = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
    if (!fn) {
        SerialPrintf("[syscall] FATAL-AVOIDED: pid=%u (%s) num=%lu has null/oob entry "
                     "(a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx) -> -ENOSYS\n",
                     proc ? proc->pid : 0, proc ? proc->name : "?",
                     num, a0, a1, a2, a3, a4, a5);
        return -38; // -ENOSYS
    }

    // Successful exec and exit never return to the post-dispatch logger. Emit
    // their attempted operation here; failed exec still gets its error below.
    if (!proc->straceErrorsOnly) {
        if (num == SYS_EXIT || num == SYS_EXIT_GROUP)
            SerialPrintf("[strace:%u] %s(%lu) [noreturn]\n", proc->pid, name, a0);
        else if (num == SYS_EXECVE)
            SerialPrintf("[strace:%u] %s [attempt]\n", proc->pid, name);
    }
    int64_t ret = fn(a0, a1, a2, a3, a4, a5);

    // In errors-only mode, skip successful syscalls entirely
    bool isError = (ret < 0 && ret > -4096);
    if (proc->straceErrorsOnly && !isError) {
        // Always log blocking syscalls from the main thread for debugging
        if (proc->pid == proc->tgid &&
            (num == 202 /*futex*/ || num == 232 /*epoll_wait*/ ||
             num == 271 /*ppoll*/ || num == 7 /*poll*/ ||
             num == 231 /*exit_group*/ || num == 60 /*exit*/))
        {
            SerialPrintf("[strace-main:%u] %s(%lx,%lx,%lx) = %ld\n",
                         proc->pid, name ? name : "?", a0, a1, a2, ret);
        }
        return ret;
    }
    bool isRepeat = (num == s_lastNum && a0 == s_lastA0 && ret == s_lastRet && isError);

    if (isRepeat) {
        s_suppressed++;
        if (s_suppressed == SUPPRESS_THRESHOLD)
            SerialPrintf("[strace:%u] ... suppressing repeated %s(%lu) = -%lu\n",
                         proc->pid, name ? name : "?", a0, -ret);
        if (s_suppressed >= SUPPRESS_THRESHOLD)
            return ret;
    } else {
        if (s_suppressed > SUPPRESS_THRESHOLD)
            SerialPrintf("[strace:%u] ... (%u identical errors suppressed)\n",
                         proc->pid, s_suppressed - SUPPRESS_THRESHOLD);
        s_suppressed = 0;
    }

    s_lastNum = num;
    s_lastA0  = a0;
    s_lastRet = ret;

    // One SerialPrintf holds the serial lock for the complete record. A split
    // prefix/result lets another CPU's syscall or debug output steal the result
    // attribution, making allocation lifetimes impossible to reconstruct.
    if(SyscallCategory(num)==SC_MEM) {
        if(num==SYS_MMAP) {
            if(isError)SerialPrintf("[strace:%u] mmap(0x%lx, %lu, 0x%lx, 0x%lx, %ld, 0x%lx) = %ld (err) comm=\"%s\"\n",proc->pid,a0,a1,a2,a3,(int64_t)a4,a5,ret,proc->name);
            else SerialPrintf("[strace:%u] mmap(0x%lx, %lu, 0x%lx, 0x%lx, %ld, 0x%lx) = 0x%lx comm=\"%s\"\n",proc->pid,a0,a1,a2,a3,(int64_t)a4,a5,(uint64_t)ret,proc->name);
        } else if(num==SYS_BRK) {
            SerialPrintf("[strace:%u] brk(0x%lx, 0x%lx, 0x%lx) = 0x%lx comm=\"%s\"\n",proc->pid,a0,a1,a2,(uint64_t)ret,proc->name);
        } else {
            SerialPrintf("[strace:%u] %s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) = %ld comm=\"%s\"\n",proc->pid,name?name:"memory",a0,a1,a2,a3,a4,a5,ret,proc->name);
        }
        return ret;
    }

    // Log entry — format depends on syscall type for readability
    if (num == SYS_OPEN || num == SYS_OPENAT) {
        const char* path = (num == SYS_OPENAT)
            ? reinterpret_cast<const char*>(a1)
            : reinterpret_cast<const char*>(a0);
        uint64_t flags = (num == SYS_OPENAT) ? a2 : a1;
        SerialPrintf("[strace:%u] %s(\"%s\", 0x%lx)",
                     proc->pid, name ? name : "?", path ? path : "(null)", flags);
    } else if (num == SYS_EXECVE) {
        const char* path = reinterpret_cast<const char*>(a0);
        SerialPrintf("[strace:%u] execve(\"%s\")", proc->pid, path ? path : "(null)");
    } else if (num == SYS_READ || num == SYS_WRITE) {
        SerialPrintf("[strace:%u] %s(%lu, ..., %lu)",
                     proc->pid, name ? name : "?", a0, a2);
    } else if (num == SYS_CLOSE || num == SYS_DUP || num == SYS_FSTAT) {
        SerialPrintf("[strace:%u] %s(%lu)",
                     proc->pid, name ? name : "?", a0);
    } else if (num == SYS_DUP2 || num == SYS_DUP3) {
        SerialPrintf("[strace:%u] %s(%lu, %lu)",
                     proc->pid, name ? name : "?", a0, a1);
    } else if (num == SYS_MMAP) {
        SerialPrintf("[strace:%u] mmap(0x%lx, %lu, 0x%lx, 0x%lx, %ld, 0x%lx)",
                     proc->pid, a0, a1, a2, a3, (int64_t)a4, a5);
    } else if (num == SYS_WAITID) {
        SerialPrintf("[strace:%u] waitid(%lu, %lu, 0x%lx, 0x%lx, 0x%lx)",
                     proc->pid, a0, a1, a2, a3, a4);
    } else if (num == SYS_LSEEK) {
        SerialPrintf("[strace:%u] lseek(%lu, %ld, %lu)",
                     proc->pid, a0, (int64_t)a1, a2);
    } else if (num == SYS_FCNTL) {
        SerialPrintf("[strace:%u] fcntl(%lu, %lu, 0x%lx)",
                     proc->pid, a0, a1, a2);
    } else if (name) {
        SerialPrintf("[strace:%u] %s(0x%lx, 0x%lx, 0x%lx)",
                     proc->pid, name, a0, a1, a2);
    } else {
        SerialPrintf("[strace:%u] syscall_%lu(0x%lx, 0x%lx, 0x%lx)",
                     proc->pid, num, a0, a1, a2);
    }

    // Log return value
    if (ret < 0 && ret > -4096)
        SerialPrintf(" = -%lu (err)\n", -ret);
    else if (num == SYS_MMAP || num == SYS_BRK)
        SerialPrintf(" = 0x%lx\n", static_cast<uint64_t>(ret));
    else
        SerialPrintf(" = %ld\n", ret);

    return ret;
}

int64_t SyscallDispatchInternal(uint64_t num, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    Process* proc = ProcessCurrent();
    SyscallTimingCall timing(proc?proc->pid:0,proc?proc->incarnation:0,num,a0,SyscallTimingPhase::Dispatch,num==SYS_IOCTL?a1:0);
    if(proc && proc->addressSpace && proc->addressSpace->pagingAdmission.Exclusive()) {
        KernelOperationGuard guard(proc);
        const int error=PageableMemoryPrepareSyscall(proc,num);
        if(error)return timing.Result(error);
    }

    if (proc && proc->straceEnabled)
    {
        // If a filter is set, only trace matching categories
        if (proc->straceFilter == 0 || (SyscallCategory(num) & proc->straceFilter))
            return timing.Result(SyscallDispatchTraced(num, a0, a1, a2, a3, a4, a5));
        // Filtered out — dispatch without tracing
        SyscallFn fn2 = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
        if (!fn2) return timing.Result(-38);
        return timing.Result(fn2(a0, a1, a2, a3, a4, a5));
    }
    SyscallFn fn = (num < SYSCALL_MAX) ? g_syscallTable[num] : nullptr;
    if (!fn) {
        SerialPrintf("[syscall] FATAL-AVOIDED: pid=%u (%s) num=%lu has null/oob entry "
                     "(a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx) -> -ENOSYS\n",
                     proc ? proc->pid : 0, proc ? proc->name : "?",
                     num, a0, a1, a2, a3, a4, a5);
        return timing.Result(-38); // -ENOSYS
    }

    // RequestServer uses the same explicitly enabled, filtered strace path
    // as every other process; no process-name-specific logging here.
    return timing.Result(fn(a0, a1, a2, a3, a4, a5));
}

// ---------------------------------------------------------------------------
// SwitchToUserMode -- enter ring 3 via IRETQ (naked)
// ---------------------------------------------------------------------------

__attribute__((naked)) void SwitchToUserMode(uint64_t, uint64_t)
{
    __asm__ volatile(
        "push %%rax\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "pushfq\n\t"
        "mov %%rbp, %%gs:24\n\t"
        "mov %%rsp, %%gs:32\n\t"
        "cld\n\t"
        "pushq $0x23\n\t"
        "push %%rdi\n\t"
        "pushq $0x202\n\t"
        "pushq $0x2B\n\t"
        "push %%rsi\n\t"
        "xor %%rax, %%rax\n\t"
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%r8, %%r8\n\t"
        "xor %%r9, %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "xor %%r15, %%r15\n\t"
        "xor %%rbp, %%rbp\n\t"
        // BRO-166: clear IF before swapgs so a LAPIC timer (or any maskable
        // IRQ) cannot fire in the ring-0 window between swapgs and iretq. In
        // that window active GS is already the user base (0); a timer landing
        // here pushes a ring-0 CS, so the ISR entry stub's conditional
        // `testq $3,CS; swapgs` would SKIP swapgs and run the handler with
        // GS=0 → #PF at gs:176 (CR2=0xB0). The hand-built frame's RFLAGS=0x202
        // re-enables IF atomically on iretq to ring 3, so this only closes the
        // kernel-side window with no change to the delivered user context.
        "cli\n\t"
        "swapgs\n\t"
        "iretq\n\t"
        ::: "memory"
    );
}

} // namespace brook

// ---------------------------------------------------------------------------
// Strace control functions
// ---------------------------------------------------------------------------

bool StraceEnablePid(uint32_t pid, bool enable, uint32_t filter)
{
    using namespace brook;
    Process* p = ProcessFindByPid(static_cast<uint16_t>(pid));
    if (!p) return false;
    p->straceEnabled = enable;
    p->straceFilter = filter;
    return true;
}

int StraceEnableName(const char* name, bool enable, uint32_t filter)
{
    using namespace brook;
    int count = 0;
    for (uint16_t pid = 1; pid < 256; pid++) {
        Process* p = ProcessFindByPid(pid);
        if (p && p->name[0]) {
            bool match = false;
            for (const char* s = p->name; *s; s++) {
                const char* a = s;
                const char* b = name;
                while (*a && *b && *a == *b) { a++; b++; }
                if (!*b) { match = true; break; }
            }
            if (match) {
                p->straceEnabled = enable;
                p->straceFilter = filter;
                count++;
            }
        }
    }
    return count;
}

void StraceEnableAll(bool enable, uint32_t filter)
{
    using namespace brook;
    for (uint16_t pid = 1; pid < 256; pid++) {
        Process* p = ProcessFindByPid(pid);
        if (p) {
            p->straceEnabled = enable;
            p->straceFilter = filter;
        }
    }
}

// ---------------------------------------------------------------------------
// ReturnToKernel
// ---------------------------------------------------------------------------

extern "C" __attribute__((naked)) void ReturnToKernel()
{
    __asm__ volatile(
        "mov %%gs:24, %%rbp\n\t"
        "mov %%gs:32, %%rsp\n\t"
        // BRO-207: normalize the restored kernel RFLAGS (clear NT|DF|AC) just
        // like context_switch — a resumed kernel continuation must not carry a
        // stale NT (which would #GP the next naked-handler IRETQ) or DF/AC.
        "andq $0xfffffffffffbbbff, (%%rsp)\n\t"
        "popfq\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "ret\n\t"
        ::: "memory"
    );
}
