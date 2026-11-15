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
