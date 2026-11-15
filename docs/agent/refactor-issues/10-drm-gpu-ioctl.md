# DRM, GPU, virtio and ioctl handling

_17 open entries (1 high, 5 medium, 11 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** DRM ioctl size/direction handling, unknown-ioctl errno, virtio PRIME/blob/execbuffer arguments, GEM allocation limits, per-file DRM lock scope, stub ioctls and `FIONBIO`/`FIOCLEX`, plus the GPU_ATTACH_WINDOW lifetime (H-22).
- **Why it matters:** M-06 can over-read and over-write user structs; M-08 lets a user-controlled size drive an uncharged allocation. The 60 fps path runs through these drivers, so changes need the Dell regression run.
- **Dependencies:** H-22 should be done together with M-51 in 11 (window lookup under `g_vfbLock`). L-13 and I-17 (errno/comments) are in 12 and 13.
- **Suggested order:** M-06, M-10 (shared dispatcher fixes), M-08, M-07, M-09, then L-07/L-08, L-50, L-11, L-52, L-51, L-67, L-68, L-49, L-06, L-12, and H-22 last (needs a virgl-capable check).
- **Test approach:** `test_syscall_user_memory/ioctl.cpp` (fake driver sees kernel buffers). Virgl behaviour is not exercised by the QEMU check; H-22 needs a virgl run or a bridge test.
- **Size:** Large: 17 entries, 2 branches (generic ioctl/DRM; virtio-gpu specifics).

### GPU_ATTACH_WINDOW lifetime (H-22)

`sys_brook_gpu_attach_window` (0xB12) hands the window's kernel VFB address to the virtio-gpu driver as resource backing. `WM_RESIZE_VFB` and window destroy then retire those pages through `CompositorRetireVfb`, while the host resource stays attached until `CtxDestroy`. Only the virtio demo apps (`gltri`, `glgears`) call it.

Proposed fix:

1. At attach, record `(ctx, resId)` on the `Window` under `g_vfbLock`, and do the window lookup under the same lock.
2. In resize and destroy, before the old VFB is retired, call `GpuAppOps::RetireResource(ctx, resId)` (detach plus host unref) and clear the record. A later use of that resource by the app then fails instead of writing freed pages.

It was not done in this pass, for two reasons:

- The driver op must not run under `g_vfbLock` if the compositor's virtio path can take that lock while holding the virtqueue. The call therefore has to move outside the lock, with a retirement pin.
- The QEMU check does not exercise virgl, so the change could not be verified.

## Entries

### High

### H-22 · gpu_attach_window gives the host a kernel VFB address with no lifetime link

- **Where:** `src/kernel/src/syscall_brook_gpu.cpp:336`, in `sys_brook_gpu_attach_window`.
- **Severity:** high (confidence medium).
- **Problem:** The VFB pages become host scatter-gather backing. WM_RESIZE_VFB or window destroy frees them while the resource stays attached until CtxDestroy, so later host transfers write freed or reused pages. Also: the lookup is not locked, and resId is not checked against the ctx.
- **Suggested fix:** Hold a backing reference for the attachment, or detach on resize and destroy. Do the lookup under VfbGuard.
- **Test:** QEMU test: attach, resize, then GPU_TRANSFER dir=1 must not corrupt foreign pages (freed-page poison check).
- **Status (fix pass):** deferred: confirmed by inspection; virtio-gpu demo path only (gltri, glgears). Design under "GPU_ATTACH_WINDOW lifetime (H-22)" below.

### Medium

### M-06 · DRM ioctl size and direction bits are ignored, so fixed-size copies over-read and over-write user structs

- **Where:** `src/kernel/src/syscall_ioctl.cpp:280`, in `sys_ioctl (DevDri) -> DrmRenderIoctl`. Also in: syscall_ioctl.cpp / syscall_drm.cpp.
- **Severity:** medium (confidence high).
- **Problem:** Only type and nr reach the handler, so sizeof(kernel struct) is always copied. An old 40-byte execbuffer gets 64 bytes read, and with FENCE_FD_OUT 64 bytes written back, corrupting 24 bytes of the caller's stack. Linux copies min(user, kernel) and zero-fills. The Intel path does check ioctlSize.
- **Suggested fix:** Pass the full cmd and honour _IOC_SIZE and _IOC_DIR.
- **Test:** Host test: EXECBUFFER with _IOC_SIZE=40 and a canary after the struct; the canary is untouched.

### M-07 · Virtio PRIME_HANDLE_TO_FD ignores and doesn't validate flags (DRM_CLOEXEC, DRM_RDWR)

- **Where:** `src/kernel/src/syscall_drm.cpp:837`, in `DrmRenderIoctl (DRM_NR_PRIME_HANDLE_TO_FD)`.
- **Severity:** medium (confidence high).
- **Problem:** dmabuf fds requested as CLOEXEC survive exec, RDWR is dropped, and unknown bits are accepted. The Intel path validates `flags&~(0x80000u|2u)`.
- **Suggested fix:** Validate the flags and derive fdFlags/statusFlags from them, sharing the Intel helper.
- **Test:** QEMU exec-inherit test for DRM_CLOEXEC; host test: flags=0x4 gives -EINVAL.

### M-08 · User-controlled rc.size drives an unbounded, uncharged backing allocation, zeroed byte by byte under the DRM lock

- **Where:** `src/kernel/src/syscall_drm.cpp:421`, in `DrmRenderIoctl (RESOURCE_CREATE) / DrmEnsureBacking`.
- **Severity:** medium (confidence high).
- **Problem:** A 1x1 texture with size=0xffffffff passes and allocates up to 4 GiB of Device pages for KernelPid, uncharged, and the client may hold 8192 handles. The zeroing loop runs under dctx->mappingLock. That is an unprivileged memory-exhaustion and stall DoS.
- **Suggested fix:** Bound the size by the geometry, add a per-file or per-process budget, and use memset.
- **Test:** Host test: a 1x1 texture with a huge size gives -EINVAL; QEMU repeated creates hit an -ENOMEM budget.

### M-09 · Virgl EXECBUFFER doesn't validate flags and ignores FENCE_FD_IN, ring_idx and syncobjs

- **Where:** `src/kernel/src/syscall_drm.cpp:760`, in `DrmRenderIoctl (DRM_NR_VIRTGPU_EXECBUFFER, non-Venus path)`.
- **Severity:** medium (confidence high).
- **Problem:** The Venus branch rejects unknown flags and syncobjs; the virgl branch accepts anything and never waits on the in-fence, which breaks cross-device ordering with a pending Intel sync file.
- **Suggested fix:** Validate the flags, wait for (or reject) a pending FENCE_FD_IN, and reject syncobj counts.
- **Test:** Host test: flags=0x80 and num_in_syncobjs=1 give -EINVAL; QEMU: a pending Intel fence is not submitted early.

### M-10 · Unknown ioctls return -ENOSYS or -EINVAL instead of -ENOTTY

- **Where:** `src/kernel/src/syscall_ioctl.cpp:884`, in `sys_ioctl`.
- **Severity:** medium (confidence high).
- **Problem:** The final fallthrough returns ENOSYS, which libraries read as "ioctl missing". The fb and dsp unknown-cmd branches return EINVAL before the 'T' catch-all, so isatty() on /dev/fb0 or /dev/dsp gives EINVAL.
- **Suggested fix:** -ENOTTY for unknown commands; let 'T' commands reach the ENOTTY path.
- **Test:** Host table test: TCGETS on fb, dsp, a file and a socket gives -ENOTTY.

### Low

### L-06 · VIRTGPU_WAIT on guest-backed BOs succeeds without waiting or checking flags

- **Where:** `src/kernel/src/syscall_drm.cpp:813`, in `DrmRenderIoctl (DRM_NR_VIRTGPU_WAIT)`.
- **Severity:** low (confidence high).
- **Problem:** A silent-success stub that relies on unrecorded synchronous Transfer3D/Submit3D. Unknown flags are accepted here, while the blob path rejects them with a raw -95.
- **Suggested fix:** Validate the flags (NOWAIT only), and add real idle tracking or assert the synchronous invariant.
- **Test:** Host test: WAIT with flags=0x2 on a legacy BO gives -EINVAL.

### L-07 · ioctl stubs that silently succeed

- **Where:** `src/kernel/src/syscall_ioctl.cpp:645`, in `sys_ioctl`.
- **Severity:** low (confidence high).
- **Problem:** Every unknown DevKeyboard cmd returns 0. TCXONC and TCFLSH succeed on any fd. TIOCSCTTY and TIOCSWINSZ are ignored. TCSETS* drop VMIN/VTIME. FIONREAD on an INET socket always gives 0. FBIOPUT_VSCREENINFO returns 0 with no VFB and never writes back. The private cmd==1 nonblock ioctl changes fde->flags without the fd lock.
- **Suggested fix:** ENOTTY for non-tty fds and unknown keyboard cmds; implement FIONREAD for TCP/UDP; write back FBIOPUT.
- **Test:** Host test: TCFLSH on a regular file gives -ENOTTY; QEMU: FIONREAD on TCP with data pending is > 0.

### L-08 · FIONBIO, FIOCLEX and FIONCLEX are handled only for some fd types

- **Where:** `src/kernel/src/syscall_ioctl.cpp:228`, in `sys_ioctl`.
- **Severity:** low (confidence high).
- **Problem:** Linux handles these generically; Brook returns ENOTTY for eventfd, timerfd, signalfd, epoll, files and DRM (libuv uses FIONBIO). The UnixSocket nonblock store is not atomic.
- **Suggested fix:** Generic handling through statusFlags/fdFlags plus per-type hooks.
- **Test:** Host test: FIONBIO on an eventfd makes read return -EAGAIN; FIOCLEX sets FD_CLOEXEC.

### L-11 · DRM_IOCTL_VERSION ignores string copyout failures

- **Where:** `src/kernel/src/syscall_drm.cpp:383`, in `DrmFillStr`.
- **Severity:** low (confidence high).
- **Problem:** A bad name pointer is skipped silently; Linux returns -EFAULT.
- **Suggested fix:** Return bool and propagate -EFAULT.
- **Test:** Host test: VERSION with a bad name pointer gives -EFAULT.

### L-12 · Duplicated GPU-context and fence plumbing with inconsistent details

- **Where:** `src/kernel/src/syscall_ioctl.cpp:205`, in `DrmRenderIoctl, sys_ioctl`. Also in: syscall_drm.cpp, syscall_ioctl.cpp.
- **Severity:** low (confidence high).
- **Problem:** Lazy CtxCreate appears 3 times, one passing config while the others pass {}. The fence-out publish appears 3 times. The 5 s wait loops differ: the DrmPrime one does unchecked addition and no terminalPending check, while the IntelPrime one uses TimeoutAddSafe and checks terminalPending.
- **Suggested fix:** DrmEnsureContext, DrmPublishFenceOut and one interruptible wait helper.
- **Test:** Host test: the DrmPrime export wait returns -EINTR on a fatal signal.

### L-49 · Per-file DRM mutex held across whole ioctls, including blocking GPU waits

- **Where:** `src/kernel/src/syscall_drm.cpp:455`, in `DrmRenderIoctl`.
- **Severity:** low (confidence medium).
- **Problem:** mappingLock is held across PresentBarrier, submits, host creation, 1 MiB user copies and the zeroing loop, and it also serializes DRM mmap and the WM present and scene lookups. One GPU wait stalls the whole process.
- **Suggested fix:** Lock only the GEM-table lookups and mutations; snapshot with lifetime references.
- **Test:** QEMU: EXECBUFFER blocked in one thread doesn't block mmap in another.

### L-50 · GET_CAP and unknown-nr semantics differ from Linux

- **Where:** `src/kernel/src/syscall_drm.cpp:484`, in `DrmRenderIoctl (DRM_NR_GET_CAP / default)`.
- **Severity:** low (confidence medium).
- **Problem:** Every capability other than PRIME succeeds with 0, including TIMESTAMP_MONOTONIC (Linux gives 1) and unknown ones (Linux gives -EINVAL). An unknown nr returns ENOTTY, where Linux gives EINVAL.
- **Suggested fix:** TIMESTAMP_MONOTONIC=1, -EINVAL for unknown capabilities, and a decision on the unknown-nr errno.
- **Test:** Host test: GET_CAP(6)=1 and GET_CAP(0xffff) gives -EINVAL.

### L-51 · PRIME_FD_TO_HANDLE imports into the host before reserving a GEM slot; bad fd gives EINVAL

- **Where:** `src/kernel/src/syscall_drm.cpp:884`, in `DrmRenderIoctl (DRM_NR_PRIME_FD_TO_HANDLE)`.
- **Severity:** low (confidence medium).
- **Problem:** When the GEM table is full, the host import is never undone. A missing fd returns -EINVAL where Linux returns -EBADF.
- **Suggested fix:** Reserve the slot first or unimport on failure; return -EBADF for a missing fd.
- **Test:** Host fake-GpuAppOps test: a full table leaves ImportResource balanced; fd 999 gives -EBADF.

### L-52 · DevIntelDri dispatch doesn't null-check the handle

- **Where:** `src/kernel/src/syscall_ioctl.cpp:273`, in `sys_ioctl`.
- **Severity:** low (confidence medium).
- **Problem:** The DevDri path returns -EBADF on a null handle; the Intel path passes null on.
- **Suggested fix:** `if(!fde->handle) return -EBADF;`
- **Test:** Host test with a null-handle DevIntelDri entry.

### L-67 · RESOURCE_CREATE_BLOB failure after Submit3D leaks a host virgl object

- **Where:** `src/kernel/src/syscall_drm.cpp:599`, in `DrmRenderIoctl (RESOURCE_CREATE_BLOB)`.
- **Severity:** low (confidence low).
- **Problem:** The blob_id create command was already submitted, and the drop path skips RetireResource when gres==0.
- **Suggested fix:** Submit a matching destroy, or reorder the calls.
- **Test:** Host fake-GpuAppOps test asserting cleanup.

### L-68 · TRANSFER and WAIT on an imported GEM run on the exporter's GPU context

- **Where:** `src/kernel/src/syscall_drm.cpp:701`, in `DrmRenderIoctl (TRANSFER_TO/FROM_HOST, WAIT)`.
- **Severity:** low (confidence low).
- **Problem:** The importer submits into the exporter's ctx (ordering and isolation), and the box is not bounds-checked. It may be intentional.
- **Suggested fix:** Use dctx->owner->ctxId and bound the box.
- **Test:** Host fake-GpuAppOps test: an import-then-transfer submits on the importer's ctx.
