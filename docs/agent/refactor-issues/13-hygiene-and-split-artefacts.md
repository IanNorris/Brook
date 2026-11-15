# Code hygiene and split artefacts

_17 open entries (1 low, 16 info). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** Dead code, orphaned or stale comments, leftover forward declarations, external symbols that should be internal, the dispatch table magic numbers, duplicated debug-frame logic, and global constructors that the kernel never runs.
- **Why it matters:** No behaviour risk for most of it; do it on a quiet branch so it does not conflict with the fix chunks. L-37 is the exception: `.init_array` is never run, so three objects' global constructors silently do nothing.
- **Dependencies:** Do last (comment edits conflict with everything). I-16 pairs with 02/10.
- **Suggested order:** L-37 (real bug), I-01, I-10, I-15, I-09, I-11, I-12, I-04, I-06, I-16, then the comment-only entries.
- **Test approach:** Build, `ctest`, no behaviour change. For L-37, a QEMU check that the constructors now run (or are replaced with explicit init).
- **Size:** Small to medium, low risk: 17 entries, 1 or 2 sittings.

### Split artefacts

These come from the move itself and are safe to clean up early, without behaviour risk:

- orphaned section banners and comments that now sit above the wrong function, or describe "the file" as the old monolith;
- the leftover declarations in `syscall.cpp` (see the info entries);
- `syscall_futex_pi.inc` and `syscall_dsp.h` comments that still refer to `syscall.cpp`;
- `syscall_mount.cpp` existing only to include `syscall_mount_namespace.inc`.

Reviewers who saw about 110 includes in some files were reading an earlier snapshot. The committed files carry pruned include lists.

## Entries

### Low

### L-37 · Kernel never runs .init_array, but three objects emit global constructors

- **Where:** `src/kernel/src/net.cpp:40`, in `(global initialisers)`.
- **Severity:** low (confidence high).
- **Problem:** linker.ld has no .init_array rule and nothing walks it. BROOK.elf nevertheless carries a 0x18-byte .init_array with entries from net.cpp, apic.cpp and serial_writer.cpp (`_GLOBAL__sub_I_*`). Today those constructors only zero .bss, which is already zero, so nothing is lost. But any future global with a non-trivial constructor would silently stay unconstructed. syscall.cpp and every split file have none (checked).
- **Suggested fix:** Either run .init_array from KernelMain before anything uses globals, or add a link-time check that .init_array is empty and make those globals constinit/constexpr.
- **Test:** A build step (like audit_entry_state.py) that fails if .init_array is non-empty, or a boot assertion that each constructor ran.

### Info

### I-01 · Dispatch table uses magic slot numbers where named constants exist; no duplicate-slot check

- **Where:** `src/kernel/src/syscall.cpp:553`, in `SyscallTableInit`.
- **Severity:** info (confidence high).
- **Problem:** No duplicate or overwritten slots and no unregistered handlers were found. But 0xB08 and 0xB09 use literals despite BROOK_SYS_WM_WINDOW_STATE and BROOK_SYS_WM_PLACE_WINDOW. Many other slots are bare numbers (0xB00-0xB07, 0xB0A, 0xB10-0xB16, 0xB40, 324, 435, 113/114/117/119, 253-255, 294). Uniqueness is checked only by scattered static_asserts, one of which lives in syscall_brook_gpu.cpp.
- **Suggested fix:** Name every slot and build the table from a constexpr {num, fn} array with a uniqueness check.
- **Test:** static_assert or host test that registered numbers are distinct.

### I-02 · Misplaced or orphaned comments

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:228`, in `sys_brook_pointer_speed`.
- **Severity:** info (confidence high).
- **Problem:** The 0xB0C taskbar comment sits on pointer_speed. The 0xB01 WM_SET_APP_ID comment sits above set_client_socket. set_crash_entry refers to "commit d" and files/crash-dump-plan.md. Both misplacements predate the split.
- **Suggested fix:** Move the comments and drop the stale references.
- **Test:** None.

### I-03 · Stale or misleading comments (memory/memfd/futex/process)

- **Where:** `src/kernel/src/syscall_memory.cpp:525`, in `sys_mremap (header comment)`.
- **Severity:** info (confidence high).
- **Problem:** Stale comments: mremap does honour FIXED; the PageCachePreload size is 16 MB, not 4; the MemFdHandleUserFault description; "SyntheticMem" in memfd_create; FdAlloc; "handled in idt.cpp"; futex_pi.inc says it is included by syscall.cpp (a split artefact); the prctl comment. retryBudget=0 is unbounded.
- **Suggested fix:** Update the comments.
- **Test:** none.

### I-04 · Dead code and duplicated logic (fork/clone, vfork wake, bin-prefix tables)

- **Where:** `src/kernel/src/syscall_process.cpp:177` (snippet matches 2 lines: 177, 275), in `sys_fork / sys_clone`.
- **Severity:** info (confidence high).
- **Problem:** sys_fork duplicates sys_clone's register block. The vfork wake appears 3 times. tgidExiting is cleared twice. There are 2 bin-prefix tables. FileMappingBacking::size is unused, and some checks are redundant.
- **Suggested fix:** Route fork through clone; shared helpers.
- **Test:** Existing fork/exec tests.

### I-05 · Orphaned or misleading comments and redundant declarations after the split

- **Where:** `src/kernel/src/syscall_common.cpp:91`, in `StrEq`.
- **Severity:** info (confidence high).
- **Problem:** The "Error codes" banner sits over signal helpers and "sys_open (2)" over StrEq in common. In poll, "Regular files are always ready" is above the Path POLLNVAL line, EpollPollReady is declared again, SC_IO is hard-coded as 1u<<5, and logPoll has no prefix. In timerfd, "using LAPIC tick count" is stale and ClockNowNs/WaitWakeupTick are declared again. EpollInstance::waiter is unused.
- **Suggested fix:** Clean up.
- **Test:** none.

### I-06 · DspHandleRef dead and duplicated; stale DSP header comments

- **Where:** `src/kernel/src/syscall_dsp.cpp:27`, in `DspHandleRef`.
- **Severity:** info (confidence high).
- **Problem:** DspHandleRef is never called; syscall_fd.cpp inlines the same increment. syscall_dsp.h comments ("further below", "defined earlier in the file") describe the monolith (a split artefact). DSP_HW_RATE and MIXER_HW_RATE are parallel constants.
- **Suggested fix:** Use DspHandleRef or delete it; fix the comments.
- **Test:** Dup/fork refcount check.

### I-07 · Misleading or orphaned comments in syscall_rw.cpp

- **Where:** `src/kernel/src/syscall_rw.cpp:1214`, in `sys_sendfile / WriteToPinnedFdByType`.
- **Severity:** info (confidence high).
- **Problem:** sendfile is not a stub. The "Shared write dispatch" comment sits above AppendUserBuffer. "The pin above" refers to the caller. The eventfd comments sit above the SignalFd and SyncFile lines.
- **Suggested fix:** Move or fix the comments.
- **Test:** n/a

### I-08 · Dead code and orphaned or misleading comments in socket files

- **Where:** `src/kernel/src/syscall_socket.cpp:89`, in `sys_socket`.
- **Severity:** info (confidence high).
- **Problem:** An empty AF_INET6 branch, 3 empty blocks, `(void)` casts after real uses, a wrong lease comment in recvfrom, UnixFdSnapRelease declared in two places and an unused UnixFdQueueFull.
- **Suggested fix:** Clean up.
- **Test:** Build.

### I-09 · Random helpers became external symbols in the global namespace

- **Where:** `src/kernel/src/syscall_common.cpp:15`, in `SoftRandU64 / RdrandU64`.
- **Severity:** info (confidence high).
- **Problem:** Both were file-static at global scope in syscall.cpp. They are shared now (getrandom and the rw /dev/urandom path), so the split dropped `static` and they are `::SoftRandU64`/`::RdrandU64`, the only split-created globals outside `namespace brook`. Moving them was out of scope (it would be a rename).
- **Suggested fix:** Move them into `brook` (or a random module, see the getrandom CSPRNG entry) in the fix pass.
- **Test:** Build only.

### I-10 · Leftover forward declarations and using-declarations in syscall.cpp after the split

- **Where:** `src/kernel/src/syscall.cpp:28`, in `(file scope)`.
- **Severity:** info (confidence high).
- **Problem:** The residual dispatcher still declares MemFdHandleUserFault/FileMapHandleUserFault and the `using brook::UserBufferReadable/...` lines from the monolith's preamble. Nothing in syscall.cpp uses them now. They were kept to keep the move pure.
- **Suggested fix:** Delete them.
- **Test:** Build only.

### I-11 · Maintenance-block reservation state is exported as raw globals

- **Where:** `src/kernel/src/syscall_maintenance_block.h:27`, in `maintenanceBlockLock et al.`.
- **Severity:** info (confidence high).
- **Problem:** open (BLK publish) and stat (temporary holders) touch the lock and counters directly, so the split had to export them. maintenanceBlockOwner stays static. The reservation protocol is now spread over three files with no single owner. This applies only to write-test and installer builds.
- **Suggested fix:** Wrap it in MaintenanceBlockPublish/MaintenanceBlockTemporaryHold helpers inside syscall_maintenance_block.cpp and make the globals static again.
- **Test:** Existing AHCI write-test QEMU path (BROOK_AHCI_WRITE_TEST_HOOKS).

### I-12 · DRM types were in an anonymous namespace but are used by six files

- **Where:** `src/kernel/src/syscall_drm.h:87`, in `DrmCtx / DrmGemObj / DrmResource`.
- **Severity:** info (confidence high).
- **Problem:** The split moved them to `brook::` (in syscall_drm.h) because memory, fd, ioctl, socket, wm and desktop all dereference them. syscall_drm.h is therefore a wide internal API (GEM table, mapping guard, PRIME, capture completion) and nothing is encapsulated.
- **Suggested fix:** Give the consumers narrow accessors (for example DrmLookupForMap, DrmPrimeRetain) and shrink the header.
- **Test:** Existing DRM/virgl QEMU tests.

### I-14 · Orphaned and misleading comments left by the split (VFS files)

- **Where:** `src/kernel/src/syscall_open.cpp:26`, in `OpenProcMemFd`.
- **Severity:** info (confidence high).
- **Problem:** "sys_write (1)" sits above OpenProcMemFd. "openat delegates to sys_open" is wrong. The renderD128 comment sits on the 129 branch. The mkdirat header is above mknodat. The access header is above forward declarations. The CloseProcessFdForced comment sits above FdReleaseOwnedEntry.
- **Suggested fix:** Move or delete the comments.
- **Test:** none.

### I-15 · Dead code and unused declarations in syscall.cpp

- **Where:** `src/kernel/src/syscall.cpp:195`, in `BrookSyscallDispatcher / file scope`.
- **Severity:** info (confidence medium).
- **Problem:** Nothing jumps to .Lsyscall_invalid. The MemFdHandleUserFault/FileMapHandleUserFault forward declarations and the `using brook::...` lines are unused after the split. `mov %%gs:16, %%r12` appears unused later (low confidence).
- **Suggested fix:** Delete them after confirming the r12 load.
- **Test:** Build plus QEMU boot.

### I-16 · Duplicated debug-frame/overlay and GEM-capture logic; inconsistent owner identity

- **Where:** `src/kernel/src/syscall_brook_desktop.cpp:363`, in `sys_brook_debug_frame`.
- **Severity:** info (confidence medium).
- **Problem:** debug_frame and desktop_overlay repeat ops 0-3 nearly verbatim. debug_frame keys the owner by thread incarnation, while 5 other sites use the leader incarnation, so a request on one thread and a read on another gets ESTALE. GEM capture validation is duplicated between wm_present_gres and scene_ctl CAPTURE.
- **Suggested fix:** DebugFrameOps(), an OwnerIncarnation(proc) helper and ValidateCaptureGem().
- **Test:** Host test: debug-frame request and read from two threads of one process both succeed.

### I-17 · Misleading comments and dead code in DRM/ioctl

- **Where:** `src/kernel/src/syscall_drm.cpp:259`, in `(file scope), DrmRenderIoctl`. Also in: syscall_drm.cpp, syscall_drm.h, syscall_ioctl.cpp.
- **Severity:** info (confidence medium).
- **Problem:** The comment claims Linux ignores the size bits; it doesn't. The "Per-fd DRM render-node state" comment in syscall_drm.h now sits above DRM_MAX_GEM rather than DrmCtx (a split artefact). The "Intel file type is not published yet" comment looks stale. DrmVirtgpuContextSetParam is unused, and GpuContextParam layout has no static_assert. The `gobj &&` null check is inconsistent with the later unchecked use. The UserBufferReadable before CopyFromUser is redundant. The refusal counters are not atomic.
- **Suggested fix:** Correct the comments, add static_asserts and remove the redundancies.
- **Test:** Compile-only.
