# Building Brook inside Brook

Use an isolated, writable experiment clone with the VM bridge enabled, eight vCPUs,
and silent host audio. Keep the normal prepared checkpoint and source checkout
separate from the test. The development toolchain package and installation
commands are in [tools/bootstrap-pkg](../tools/bootstrap-pkg/README.md).

The test has distinct milestones: guest package installation, a real HTTPS Git
clone and the two required submodules, building the kernel/bootloader/modules
and bare-metal tests in the guest, running hosted tests in Brook, then booting
the resulting guest-built kernel. A host-exported binary SDK supplies tools;
that does not imply that Nix or the compiler was rebuilt inside Brook.

## Next acceptance goal

The native compatibility gate now contains 44 suites. Both prepared-SDK and
clean-image paths passed at pinned revision `ead55379`, including guest-built
boot and repeated tests. The [44-suite refresh below](#44-suite-bootstrap-refresh-18-october)
records exact artifacts, timings, profiling limits and launcher evidence scope.

Maintain two acceptance paths, using isolated disks and the same pinned Brook
revision, SDK closure and VM configuration:

- **Prepared image:** start with the SDK already installed, clone Brook and
  its required submodules, build, run hosted tests and boot the guest outputs.
- **Clean image:** start with the base Brook image and its guest installer,
  install the same SDK through `nix-install`, then execute the same steps.
  This tests package download and extraction as well as source compatibility;
  it does not require building the SDK or Nix from source inside Brook.

Record wall time separately for SDK installation, clone/submodules,
configuration, compilation/linking, hosted tests and guest-built boot.
Capture CPU samples, context-switch activity, used/free memory and available
fault/I/O counters around the expensive stages. Record host load and VM
settings alongside results. Keep cold and warm cache runs distinct, collect
the baseline before optimising, and repeat the expensive stages enough to
distinguish stable bottlenecks from run-to-run variation. Retain exit statuses,
logs, source/toolchain identifiers, artifact hashes and synced offline-fsck
results. Both paths must boot their own outputs and pass the guest tests to
qualify as complete acceptance runs.

### Reproducing the two paths

Run these host commands from the repository inside `nix-shell`, after building
the current Release kernel and fetching its userspace dependencies. Use a full
Git revision available on the remote; both paths must use the same revision.
Output directories must be new. Seeds contain no Quake data.

```sh
python3 scripts/export_guest_package.py --expression tools/bootstrap-pkg \
  --name brook-bootstrap --version 1
python3 scripts/create_experiment_disks.py --bootstrap-mode clean \
  --nix-gib 12 --data-gib 8 --output build/experiments/seeds/bootstrap-clean
python3 scripts/create_experiment_disks.py --bootstrap-mode prepared \
  --nix-gib 12 --data-gib 8 --output build/experiments/seeds/bootstrap-prepared
revision=$(git rev-parse HEAD)
python3 -u scripts/bootstrap_acceptance.py --mode clean \
  --seed build/experiments/seeds/bootstrap-clean \
  --output build/experiments/results/bootstrap-clean --revision "$revision" --backend ram --memory 4G
python3 -u scripts/bootstrap_acceptance.py --mode prepared \
  --seed build/experiments/seeds/bootstrap-prepared \
  --output build/experiments/results/bootstrap-prepared --revision "$revision" --backend ram --memory 4G
```

The clean seed includes Bash, coreutils, curl, xz, zstd, certificates and freshly
compiled static guest package tools. It contains no SDK. The clean harness
asserts the actual guest store contains exactly the base closure before install,
and that its compiler and SDK shell are absent. The prepared seed
adds the exported SDK's exact closure. The harness starts its local HTTP binary
cache. SDK-only files in the prepared image use the installer's 0644/0755 modes,
rather than the host store's 0444/0555, so mode-dependent mmap cache behavior
matches. Base transport paths keep the same host-prepared modes in both images.
The harness runs the real guest installer, performs an HTTPS clone and pinned
submodule update, and builds kernel/EFI/modules/bare-metal/native artifacts.
It validates module imports and requires the explicit native-suite inventory
(currently 44/44 CTest results) twice, before and
after a clean native rebuild with warm caches. The incremental kernel stage
is labelled separately from a full compilation.

`--backend ram` keeps the main boot/data/Nix payload on private host tmpfs.
The harness budgets the exported SDK's uncompressed NAR closure (or larger
seed allocation), another GiB for filesystem/boot/build overhead, guest memory
and a configurable host reserve (`--host-headroom-gib`, default 4 GiB).
Disk-backed runs also require guest capacity plus that reserve. The harness
checks memory again before making independent
verification images. Successful images are saved as sparse regular files at
completion, then only the run's owned RAM directory is removed. Failed runs
retain their images for diagnosis. Tmpfs can swap, and auxiliary firmware
files may still use `/tmp`; this is not a zero-SSD-write guarantee.

After a host OOM, prefer `--backend disk` for the larger bootstrap runs and
run one VM at a time. Cap host builds with `CMAKE_BUILD_PARALLEL_LEVEL=4`.
Failed RAM runs retain their images and can continue occupying RAM or swap
after QEMU exits: preserve them as sparse regular files and remove only their
owned runtime directory after confirming every associated VM is stopped.
Lower the host reserve explicitly only for a host with an appropriate budget.

`--resume-completed-build` only resumes host verification after a guest build
already exited zero with both expected native-test summaries and artifact markers. It does
not turn a failed or hung guest build into acceptance. Shutdown evidence records
whether sync was acknowledged live or came from an already completed script
and a normal launcher-recorded exit.

New runs separately persist and require zero from the owned VM launcher in
`launcher-exit.json`, before extraction and after verification. Resume requires
that matching receipt; a QEMU exit alone does not substitute for it. Two host
tests use actual subprocesses to verify zero acceptance and nonzero rejection.
Historical runs without that receipt retain their explicitly narrower evidence.

Each new run uses eight vCPUs, eight build jobs, 4 GiB by default, software virtio graphics, silent audio and the
kernel WM command listener without Wayland apps. The kernel compositor still
runs: this is a no-Wayland baseline, not a machine with no graphics work.
Unrelated home/data disks are omitted; USB and media fixtures are private to
each run. Host page cache is uncontrolled. `progress.json` retains periodic memory/load
observations; `stage-results.json` retains guest wall seconds and host marker
times. Long stages also receive a 3-second CPU/context-switch capture; a
capture near a stage boundary can include subsequent work. Raw profiles and
Speedscope output reside in the VM registry. Module addresses must not be
interpreted using the nearest kernel symbol.

After guest sync and shutdown, the harness requires offline fsck on both
disks, extracts artifacts with debugfs, hashes them, boots their kernel/EFI
and modules in a second isolated VM, verifies the booted kernel hash and
command replies, and reruns Git fsck and all 44 native tests under that kernel,
then repeats synced offline fsck. Only this complete sequence
writes `result.json` with `success: true`. Failure retains logs/disks and may
leave the VM running for diagnosis; use its bridge session to sync and stop it.

### Initial measurements and compatibility progress

A serial build of the 13 hosted tests in the desktop guest took 76 seconds
including clean, compilation and linking. This is a preliminary measurement
from the existing experimental disks, not either fresh acceptance path. A
10-second capture retained 1,000 CPU samples and 2,466 context switches:
133 samples were in user mode, 534 in kernel code and 333 in loaded modules.
Compiler PID 18 accounted for 448 samples; compositor PID 12 accounted for
336. Kernel samples include page-fault handling, allocation and virtio block
I/O. Module addresses need module-specific symbols: the current converter's
nearest-kernel-symbol labels for those addresses must not be used as evidence.
The capture's memory observation was 343,613,440 bytes used out of
8,536,883,200 bytes total. Compare a console-only configuration with this
desktop run before attributing elapsed time solely to compilation.

The native startup failure was caused by Nix library lookup substituting the
boot disk's musl for a missing `libc.so` during linking. Exact store lookup and
rebuilding fix that. CTest then exposed a lost SIGCHLD notification: a child
leader could exit before its final thread was reaped, so WNOHANG returned zero
and no new notification arrived when the leader became waitable. Releasing the
thread's address-space hold at the end of teardown under the scheduler lock
now notifies the parent at that transition and protects the leader pointer.
Post-switch publication of `reapable` also notifies the parent when no threads
remain, covering either ordering of the two waitability gates. With both
transitions covered, ten repeated CTest mutex runs pass in 3.91 seconds without
the event-loop stall.

The initial one-vCPU guest run completed with 10/13 tests passing. Positive
sub-millisecond nanosleep now rounds up and rechecks a monotonic deadline;
the duration/invalid-timespec guest probe passes. The reader test explicitly
rendezvouses while holding read locks, with a bounded escape: a deliberately
serialized implementation fails its overlap assertion without hanging.

The original descriptor stress exceeded a 600-second diagnostic deadline.
Its user samples are concentrated in the pin-user and pin-closer ticket-lock
spin loops. Workers now rendezvous before starting, and yield outside fd-table
operations, including while holding a descriptor pin so closers exercise
deferred teardown. All 500 close rounds, 800 pin rounds, 16 competitors per
round, real fd-table/ticket-lock code and ownership/UAF assertions remain.
That is 20,800 thread creations, not just the first phase's 8,000.

With these changes, all 13 guest tests pass in 63.90 seconds on one vCPU;
descriptor stress takes 54.57 seconds. The host suite with the stronger
concurrency coverage passes in 22.11 seconds. This improvement applies to
the test workload's scheduling, not a claim that compilation or Brook as a
whole became ten times faster. The fresh two-path bootstrap and offline-fsck
acceptance runs remain outstanding.

The first newly formatted lifetime-test disk exposed two uninitialized ext2
cache locks. Before the fix, boot stalled in `Ext2ReadInode` with inode-cache
ticket counters `0xb10cbef0` and `0x000bd7c0`. Explicitly initializing both
inode and directory-entry cache locks restores fresh-disk boot; 13 host tests
also pass. The isolated boot-stall disk checked clean before reuse.

On that initially fsck-clean data disk, `filelifecheck` then reports eight
failed checks before stopping at directory cleanup: open orphan inode reuse,
lost original contents/link metadata, orphan writes altering a replacement,
an unfaulted mapping reading replacement contents, overwritten-destination
link metadata, nonempty rmdir and directory unlink. After a normal synced
shutdown, `e2fsck -fn` exits 4 and lists unattached inodes from the 32 rename
replacements. This is the pre-fix baseline, not a completed filesystem gate.
The before image, serial evidence and offline check are retained under
`build/experiments/results/ext2-lifetime-20261014`.

## Compatibility defects found during preparation

The first real Git clone found that BusyBox fallback advertised every unknown
system-bin command as an executable. Git stopped PATH search before reaching
its real remote helper. The fallback now checks the shipped static BusyBox's
actual applet names. `scripts/generate_busybox_applets.py ./busybox_static`
regenerates the allowlist when replacing that binary; rebuild the kernel too.

Git's ordinary failure exit code 128 was also reported as successful: wait4
inferred signal death from every code >=128, turning 128 into zero. Termination
now records Linux wait status explicitly, preserving all eight normal exit bits
and distinguishing signals. The `waitcheck` probe validates raw exit/exit_group
codes 0/1/127/128/255, SIGTERM/SIGKILL and Bash `set -e` external-child behaviour.
The process state release stores also use the field's actual byte width.

The real kernel link command contains over 100 arguments. Previous exec copying
and stack setup silently capped vectors at 64 entries. Shared limits now allow
512 arguments, 256 environment entries, 64 KiB argument strings and 32 KiB environment
strings. Invalid pointers return EFAULT and count/byte overflow returns E2BIG
before image replacement. Shebang argument insertion is checked and private;
it no longer uses a shared scratch buffer or discards trailing arguments.

`execcheck` exercises 128/256 arguments, over 48 KiB strings, exactly 512 argv and 256
env entries, a 20 KB single string, count/byte overflow, unmapped-page pointer and
string boundaries, a NUL at the final mapped byte and shebang preservation.
Both probes passed in isolated Brook regression guests, as did 13 host tests.
Their sources and invocation examples live in [tools/at-syscall-probe](../tools/at-syscall-probe/README.md).

The next clone attempt exposed filesystem metadata defects: path `statx`
invented mode `0644` and inode `1`, and all stat variants dropped ext2's stored
timestamps. Git uses executable mode bits to find remote helpers, while Ninja
needs modification times. Stat variants now share real inode metadata; ext2
updates creation/write/truncation times and supports `utimensat`/`futimens`
with whole-second precision. Old files legitimately retain their old on-disk
timestamps until repaired with a real guest `touch`.

Reporting the actual permissions then exposed a package creation defect:
`open(O_CREAT, mode)` discarded the requested mode, leaving NAR executables
at `0644`. The installer itself already requests `0755` for executable files.
Creation must preserve that mode after applying the guest umask, and mode
updates must dispatch through the file's actual mount rather than the first
ext2 mount. Existing SDK files need their original executable metadata restored;
do not mark the entire Nix store executable.

Always quiesce guest writers and sync before stopping or snapshotting a VM.
Ext2 buffers allocation bitmaps while writing inodes and directory entries
immediately. An unsynced QMP quit can leave allocated inodes marked free, so
the next boot can reuse them and corrupt existing files. Normal bridge stop
and reset must await the synchronous guest sync reply; forced shutdown is
explicit and cannot promise a consistent disk. This flushes guest metadata
caches under normal I/O, not a journaling or power-loss durability guarantee.

The metadata and mode probes passed in Brook, with mode tests on both data
and Nix mounts. All 13 host tests and 26 bridge tests passed. Recovery used
independently `e2fsck -fn`-clean checkpoint copies; damaged disposable images
were retained for diagnosis. The installed SDK's 1,083 executable files were
identified from its original binary closure, rather than guessed from names.

Git's threaded DNS resolver also exposed an indefinite `pselect6` wait after
its worker wrote the eventfd notification. Select/pselect now use poll's
readiness and waiter handling instead of treating eventfds and sockets as
always ready. `selectcheck` passed in Linux and Brook, including delayed
eventfd/pipe writes, TCP accept/data, timeout units, high descriptors and
unmapped-buffer boundaries. Brook's TCP test uses its configured NIC address;
127/8 loopback routing is still missing. The probe uses a separate parent
watchdog because Brook does not implement `alarm` yet.

After object transfer, Git found phantom references: opening a missing refs
directory with a trailing slash fell back to `/boot/lib/`. Basename fallback
is now limited to read-only shared-library names under intended library/Nix
prefixes; missing directories and unrelated names fail normally. `opencheck`
passed in Brook, and a fresh guest `git init` plus `git fsck --full` succeeded
without phantom references. The existing exact `/boot` aliases remain supported.

The main HTTPS clone then succeeded, but submodule update repeatedly registered
URLs and still considered them uninitialized. Raw `access(".git/config")`
looked up the file under `/` instead of the caller's cwd, although absolute
reads and mmap agreed on the valid configuration. Access variants now resolve
cwd/dirfd paths, and classic symlink/rmdir/rename use cwd too. `accesscheck`
passed all 26 assertions in Linux and Brook, including relative rename and
cleanup. Retrying the existing clone fetched both pinned submodules and verified
`vendor/uefi-headers/Include/Uefi.h` and `src/third_party/fatfs/source/ff.c`.

Access permission-bit evaluation and long stored directory-fd paths remain
separate limitations; these checks validate pathname resolution and existence.

CMake's child-process event loop exposed caught SIGCHLD being discarded by
blocking pipe/tty/AF_UNIX reads. Reads now ignore only default/ignored SIGCHLD,
preserve installed handlers and clear their wait registration on interruption.
Epoll checks pending signals before sleeping. The five watchdog-protected
`childsignalcheck` cases passed in Linux and Brook, including exec-child epoll
notification, pipe/socket interruption, SA_RESTART and default-ignored SIGCHLD.

The guest build reached compiler checks and font baking, then found mtools
missing from the SDK. The package installer successfully installed it in the
guest; the fresh SDK expression now includes it. The FAT-image script also
reports the failing mtools command and stderr instead of only a subprocess
exception. Its host verification produced the complete 368,640-byte image.

Mformat then exposed fd 3 writes being unconditionally sent to UART after
Python closed and reused that descriptor for its image file. Debug serial now
has an explicit descriptor type; reused/duplicated descriptors follow their
actual type. `fd3check` passed on Linux and Brook, including exact file bytes,
pwrite/writev, pipe/eventfd and exec reuse, with both legacy serial sentinels
confirmed in UART output. Existing enum values and descriptor layouts stay
unchanged; driver header dependencies rebuilt the affected modules.

Runtime evidence is retained under ignored `build/experiments/results/bootstrap-20261014/`
and `bootstrap-compat-20261014/`, and the VM registry keeps serial logs and the
actual booted kernel identities. Package payloads, images and logs stay out of git.

## Filesystem lifetime gate (2026-10-14)

The final isolated guest run passed all 24 `filelifecheck` assertions, including
actual directory inode reuse (`old=18 new=18`), independent open descriptions,
an unfaulted mapping, a forked descriptor, open rename victims, cross-parent
directory moves, inline/block-backed symlinks and an 8 MiB orphan grown through
double-indirect blocks. After acknowledged guest sync and shutdown, offline
`e2fsck -fn` exited zero: 14 files and 9,007 blocks, identical to the initial
formatted disk. Evidence is retained in the ignored lifetime experiment and
bridge session `787747cd981e4162bd2fd7b52938b38c`.

Ext2 now tracks live inode holders, defers orphan reclamation until their last
close, and drains that queue on sync and namespace writers. Reclamation
preflights block trees and persists detached pointers before releasing bitmap
allocations. Rename replacement accounts for its victim; typed removal checks
empty directories and maintains parent links. Real link counts, typed removal
and busy-unmount callbacks require module ABI 4; rebuild kernel and modules
together. Existing open holders prevent unmount. Full concurrent mount-operation
leases remain a separate follow-up. This gate does not claim crash atomicity
or recovery from injected partial I/O failures; initial creation writes and
failed rollback writes need further audit.

Fresh clean-image preparation also exposed false cross-mount file identity:
coreutils `cat` rejected `/nix/index/packages.idx` as its `/data` output because
both files had equal inode numbers and stat hardcoded device 1. Ext2 now
reports a unique virtual device per mount; statx reports the matching major
and minor. The five-check `devicecheck /data /nix` probe passes with both
roots at inode 2 and distinct devices, and the host suite passes 13/13.
This extends `VnodeStat` and advances the combined module ABI to **5**; use
a matching kernel and rebuilt modules.

### I/O pivot checkpoint (2026-10-15)

The fresh clean-image diagnostic successfully installed all 91 SDK additions
in 2,494 seconds and completed its HTTPS shallow clone in about 253 seconds.
It did not finish the complete build/boot acceptance sequence. After quiescing
writers and acknowledged sync/shutdown, the Nix disk passed offline fsck; the
data disk reported one inode `i_blocks` mismatch for `test_vlc_panic.gif`.
Its contents matched the pinned Git revision, but the inode reported 536
sectors instead of 568. A virtio timeout/reset appears during checkout; an
ignored final inode-write failure is a supported hypothesis, not an identified
failed request. The original disk is preserved, with no automatic repair.

The immediate prerequisite is the [fast I/O gate](io-benchmark.md). At this checkpoint, both full prepared/clean acceptance runs remained outstanding;
the later clean-image result is recorded below. Historical timings above
used one vCPU; new launch/build defaults use eight. The metadata-batching
candidate is on an experimental branch because small-file performance still
regresses despite much faster sequential writes.

### Clean-image acceptance (2026-10-15)

The RAM-backed clean-image run at revision `ee03703` passed the complete
sequence with eight vCPUs and 4 GiB guest RAM. The base image contained only
39 transport paths; guest `nix-install` added the SDK in 143 seconds. The
HTTPS clone took 24 seconds, checkout 12 seconds, submodules 27 seconds, and
the full kernel build 121 seconds. All 13 native tests passed both before and
after a clean native rebuild, then passed a third time under the guest-built
kernel. Git fsck and offline checks of both disks also passed. Peak sampled
guest memory usage was about 775 MiB.

Evidence is retained under ignored
`build/experiments/results/bootstrap-clean-ram-20261015/`, including the
success result, stage timings, profiles, artifact hashes and filesystem
checks. The prepared-SDK acceptance run at the same revision is still pending;
this result does not establish acceptance for both paths.

### Prepared-image wait diagnosis (2026-10-15)

The same-revision prepared run completed clone, checkout and submodules, then
stalled with Ninja in `ppoll` and its CMake child already exited and reapable.
Ninja supplies a temporary mask to unblock SIGCHLD during this wait; Brook
ignored that argument. A non-destructive thread dump established the blocked
parent and exited child. Guest sync and shutdown succeeded, and both preserved
filesystems passed offline checks. This was not a completed acceptance run.

The fix implements temporary-mask delivery/restoration for ppoll, pselect6 and
epoll_pwait, including EINTR despite SA_RESTART. Its 27 guest regression cases
pass, as do five child-signal and 24 readiness cases. Timeout tests additionally
exposed early expiration against LAPIC ticks; wait expiration now uses the
monotonic clock. Full prepared/clean acceptance will be rerun at one matching
revision with these changes.

### Native test iteration cost (2026-10-15)

The descriptor stress test now reuses worker threads across rounds rather than
creating and joining 20,800 threads. It keeps all 500 close-race rounds, all 800
pin/close rounds, worker counts, random seeds and correctness assertions. Start
and end barriers isolate each round's reset from competing workers.

On the host, the descriptor target fell from about 64 seconds to 1.3 seconds;
the complete 13-test Release suite passed in 2.49 seconds. Guest timings still
need measurement in the next bootstrap runs. A ThreadSanitizer run was stopped
after more than 15 minutes of CPU saturation and is incomplete, not a passing
sanitizer result. Its instrumented ticket-spin iterations perform runtime
synchronization work; that observation does not establish race freedom.

The newer prepared-image run built the kernel in 53 seconds and passed both
native test runs, but failed offline checking with ten leaked ext2 blocks.
The mixed-append cache regression and its validated fix are described in
[io-cache-regression.md](io-cache-regression.md). Fresh prepared and clean
acceptance remains required at one matching revision.

### Worker-page ownership diagnosis (2026-10-15)

The next prepared run at `95aa4f6` stalled in Git during kernel configuration.
Git's allocator mapping at VA `0x24000000` still pointed to physical frame
`0x16efe000`, but serial logs recorded that exact frame being freed by
`PmmKillPid(19)` with mapCount 1. Its captured contents had subsequently become
machine code while Git remained asleep on the former mutex at offset 0x30.
The failed VM stopped with a checked sync acknowledgement; FAT and both ext2
filesystems checked clean. This remains a failed build, with preserved images
and diagnostics under `bootstrap-prepared-95aa4f6-ram`.

User backing pages allocated by workers were charged to their recyclable thread
IDs. Those mappings outlived the allocating thread, and an unrelated process
reusing its ID could reclaim them on exit. User backing and PTE ownership now
use the stable thread-group ID; page-table pages remain kernel-owned. This
changes neither Process layout nor module ABI and retains the PMM diagnostics.

The focused guest regression retains anonymous, mprotect, private-file and COW
mappings after worker exit. A separate, pre-existing process reserves child IDs
in batches and must demonstrably reuse the worker ID. The old kernel corrupted
the anonymous, mprotect and COW contents after reuse. The fixed kernel preserved
all bytes, with observed reuse, and passed the complete I/O gate with flush
failure/retry, reboot verification, clean FAT/ext2 checks and exact allocation
recovery in 27.31 seconds. All 13 host and seven bare-metal tests also passed.
Evidence is under `build/io-bench/thread-pages-old-negative-r2` and
`thread-pages-fixed-positive-r2`.

Acceptance now rejects mapped-page leak/underflow diagnostics even if the guest
reports build success. Profiling transport timeouts disable further profiling
and are recorded as incomplete observations; they do not restart the VM or
relax the required native-test, artifact, own-kernel boot or filesystem checks.
Fresh prepared and clean acceptance runs are still required after this fix.

### Thread-group exit diagnosis (2026-10-15)

The prepared run at `e26f968` reached linking without mapped-page warnings,
then stalled: the terminated `ld.lld` leader retained two blocked, reapable
workers and an address-space live-thread count of two, with no registered
futex waiters. The VM stopped with checked synchronization; FAT and both ext2
checks passed. Its failed-run images and thread dump are retained under
`bootstrap-prepared-e26f968-ram`.

Group termination previously published state outside the ready-queue lock,
allowing a concurrent block/wake/preemption transition to overwrite Terminated.
Terminal publication and scheduler transitions now use the same lock. Futex
waiters own process references, cancellation follows off-CPU quiescence, and
wake/removal preserves those references through interrupt-sensitive transfer
windows. Birth and signal/wakeup snapshots also follow the scheduler lock
order. Process layout and the module ABI are unchanged.

The old kernel timed out in round 3 of the guest `exitgroupcheck` probe. The
candidate passed separate 128- and 512-round runs, all 13 host tests and seven
bare-metal tests. The full I/O/durability gate passed with the longer stress
run in 30.66 seconds. A further 128-round gate verified guest process records
returned from 7 to 7 after worker reaping. No mapped-page or quiescence-timeout
diagnostics appeared. Results are under `build/io-bench/exit-group-*`.

These tests establish the targeted fix, not general waiter lifetime safety:
raw pipe/eventfd/socket waiter pointer lifetime and ABA remain unverified.
The existing emergency cleanup after an exit-group quiescence timeout also
remains risky; both acceptance harnesses reject that warning. Fresh prepared
and clean acceptance at one matching revision remain required.


### Prepared acceptance after the scheduler fixes (2026-10-15)

The prepared-SDK path passed the complete acceptance sequence at
`8042310e80b2cff58fcaabf5a455b9ccadd11a34`, using eight vCPUs, 4 GiB guest
RAM and disk-backed isolated images. The instrumented guest stages reported
45 seconds for the kernel build, 12 seconds for native-test compilation,
5 seconds for each of the cold and rebuilt native-test runs, and 2 seconds
for the incremental build. These are one run's measurements, with uncontrolled
host caches, rather than a matched performance comparison.

All 13 native tests passed in both build-stage runs and again after booting
the extracted guest-built kernel with its matching modules. Extracted artifact
hashes were verified. FAT and both ext2 images passed offline checks after the
build VM stopped and after the independent guest-built verification boot.
The guest-built kernel SHA-256 was
`25245eb675928830628d7788fb4bf439b2e1a639d9dcd615d138456f048e3728`.

Evidence is retained under ignored
`build/experiments/results/bootstrap-prepared-8042310-disk/`.
The clean-image path then passed the same complete sequence at this revision,
including installing the SDK through guest `nix-install` in 36 seconds. Its
kernel build took 42 seconds, native compilation 11 seconds, both native-test
runs 5 seconds each, and the incremental build 2 seconds. Its independent
verification boot passed the third 13-test run, with all six final filesystem
checks returning zero and extracted artifact hashes verified. Evidence is
retained under `build/experiments/results/bootstrap-clean-8042310-disk/`.
The clean run's guest-built kernel SHA-256 was
`d6d3f882688f13deecfb9fc52e66b1584ad71c540bd856c5a5da508582ca6c01`.

This completes the matching two-path bootstrap acceptance milestone. Both VMs
were stopped after verification. These single-run timings include profiling
and do not establish cache-controlled performance or reproducible binaries.
Further real-workload compatibility and continuous VM recording remain separate
work. After the host client OOM, these runs used one disk-backed VM at a time;
the final host check showed about 14 GiB available and no remaining test VM.

### Later compiler-exit regression (2026-10-15)

A new prepared run at `9942505e4e9338704dae7c22b78a1f557dcfff03`
completed clone, checkout, submodules and configuration, then stalled during
compilation. The FAT mutex still named PID 16, incarnation 535, after that
LLVM worker had been reaped. Python was blocked acquiring the same mutex;
the CPUs were idle. This was a lock-lifetime failure, not a slow build.
Scheduler termination could discard a sibling's kernel stack while it held
or awaited a sleeping filesystem lock.

The failed VM could not acknowledge sync and was forcibly stopped. Read-only
offline checks passed for boot and Nix; the unsynced data image reported
allocation-bitmap errors. Images are retained without repair under
`build/experiments/results/bootstrap-prepared-9942505-disk/`, alongside
`failure.json` and the final serial log. This run failed acceptance. The clean
path was not run at that revision; the matching successful pair above applies
only to `8042310`.

The successor defers fatal cancellation while a thread holds or awaits a
sleeping filesystem lock, retaining protection until the complete kernel
operation unwinds. Lock acquisitions and wake handoffs pin process identities;
group-exit timeouts retain live operations instead of force-unlocking metadata.
Timer reaping runs only at user/idle boundaries in its own protected operation.
Process-directed positive-PID `SIGKILL` now terminates the target thread group,
including when sent by its leader or another worker. Process and mutex layout
changes require rebuilt modules with ABI 8.

The focused guest gate passed 16 rounds with eight I/O workers on each of FAT
and ext2, covering `exit_group`, external SIGKILL and leader/worker self-group
SIGKILL. Truncation gates passed on both filesystems, all four retained fixtures
verified after reboot, and checked shutdown followed by offline FAT/data/Nix
checks returned zero. Kernel heap integrity also passed its new free-payload
accounting check; all 13 hosted tests passed in 1.76 seconds. Exact hashes,
sessions and logs are retained in
`build/workloads/icons-theme-20261015/fsexit-acceptance.json`.
These focused results do not replace full prepared and clean bootstrap
acceptance on the successor. Thread-directed fatal-signal group semantics,
process-group kill selectors and operation boundaries before unrelated blocking
waits remain follow-up work.

### Matching acceptance after cooperative cancellation (2026-10-15)

Both paths passed the full sequence at
`e70ff31dc7656fb6c3ee38e7d6d6243767930628`, with eight vCPUs, 4 GiB RAM,
disk-backed isolated images, software graphics and silent audio. Each cloned
the pinned source, built Brook, passed all 13 hosted tests before and after a
clean native rebuild, then booted its own extracted kernel/EFI/modules and
passed all 13 tests a third time. Both build VMs acknowledged checked sync;
all twelve final offline checks across the build and verification images
returned zero. Every VM was stopped after its checks.

| Guest stage | Prepared SDK | Clean image |
|---|---:|---:|
| SDK installation through nix-install | Already installed | 42 s |
| HTTPS clone | 11 s | 13 s |
| Submodule update | 19 s | 19 s |
| Kernel/module/test build | 44 s | 39 s |
| Native-test compilation | 12 s | 10 s |
| Native-test run | 5 s | 5 s |
| Warm native rebuild | 11 s | 8 s |
| Warm native-test run | 5 s | 5 s |
| Incremental kernel build | 2 s | 2 s |

These are individual instrumented runs with uncontrolled host caches, not a
matched speed comparison. Raw CPU/context-switch captures accompany expensive
stages; use `scripts/bootstrap_perf_report.py` to summarize them without
attributing unresolved module addresses to unrelated kernel symbols.

Evidence is retained under
`build/experiments/results/bootstrap-{prepared,clean}-e70ff31-disk/`.
The prepared guest-built kernel SHA-256 is
`36c34fdaf94b12d06e97910cec88be250fd79024ce56a1f3fd27a94f9e33b5c9`;
the clean counterpart is
`f3aef5bb45c7369903a009083962440098bac122a7eda7a6656985b80dad2b62`.
Artifact hashes are checked against the booted copies, rather than claiming
byte-identical reproducible builds between the two paths.


### Matching acceptance after browser compatibility fixes (2026-10-15)

Both paths passed again at `e5dd9aa862cc3542886823aec8c624bd69ef85d2`,
including the pending Unix connection lifetime fix, memfd permissions and
preallocation, user GS preservation, and ext2 copyout/cache-lock correction.
The latter has a separate concurrent CoW/file-fault regression that reproduced
the previous kernel's deadlock and passes the fixed kernel.

Each acceptance run used eight vCPUs, 4 GiB RAM, isolated disk-backed images,
software graphics and silent audio. Each passed all 13 hosted tests twice,
then booted its own extracted kernel/EFI/modules and passed all 13 again.
All four VMs stopped with checked sync; all twelve final offline filesystem
checks returned zero. The clean image installed its SDK through guest Nix.

| Guest stage | Prepared SDK | Clean image |
|---|---:|---:|
| SDK installation through nix-install | Already installed | 43 s |
| HTTPS clone | 16 s | 11 s |
| Submodule update | 20 s | 19 s |
| Kernel/module/test build | 38 s | 37 s |
| Native-test compilation | 8 s | 7 s |
| Native-test run | 5 s | 6 s |
| Warm native rebuild | 7 s | 7 s |
| Warm native-test run | 5 s | 5 s |
| Incremental kernel build | 2 s | 2 s |

These are individual instrumented runs with uncontrolled host caches. They do
not establish a speedup over earlier runs. The clean run disabled further
profiling after an export timeout; its completed build/test timing markers
remain available. Prepared captures also dropped records, so their retained
sample proportions are not an unbiased CPU-time breakdown.

Evidence: ignored `build/experiments/results/bootstrap-{prepared,clean}-e5dd9aa-disk/`.
The prepared guest-built kernel SHA-256 is
`18db398ad1b336c1114653ad9bb279223ff35a8a1dffab5ea662de5df04688de`;
the clean counterpart is
`42156b8d88e71fd91d3a9670d8676614722798535b7c9b3a5282fc41b10ed081`.
The verifier checks these hashes against the booted kernels. Browser/editor
usability and general writable shared-file mappings remain separate workload
requirements.


### Matching acceptance after atomic child waits (2026-10-15)

Both full paths passed at `94121ca7aac52320ed82ef6f0400e9558837f4bd`, including
atomic wait4/waitid event consumption, parent thread-group ownership and the
stopped-task CPU handoff correction. Both used eight vCPUs, 4 GiB RAM,
disk-backed isolated images, software graphics and silent audio. Each cloned
the pinned revision through HTTPS, built Brook, passed all 13 host tests twice,
then booted its exact extracted kernel/EFI/modules and passed all 13 again.
All four VMs stopped with checked synchronization and all twelve final
read-only filesystem checks returned zero. The clean image installed its SDK
through guest `nix-install`.

| Guest stage | Prepared SDK | Clean image |
|---|---:|---:|
| SDK installation | Already installed | 38 s |
| HTTPS clone | 10 s | 12 s |
| Submodule update | 21 s | 19 s |
| Kernel/module/test build | 38 s | 38 s |
| Native-test compilation | 8 s | 8 s |
| Native-test run | 5 s | 5 s |
| Warm native rebuild | 7 s | 8 s |
| Warm native-test run | 5 s | 4 s |
| Incremental kernel build | 0 s | 2 s |

These are individual instrumented runs with uncontrolled host caches and
whole-second guest stage timestamps; a zero means below that reporting
resolution. They do not establish a speedup. Prepared profiling was disabled
after an export timeout. Clean captures succeeded, but several dropped records;
retained proportions from those captures cannot be treated as an unbiased CPU
time breakdown. The three-second install capture retained 2,411 CPU samples
with zero dropped records; it covers part of installation, not its full duration.

Evidence is under ignored
`build/experiments/results/bootstrap-{prepared,clean}-94121ca-disk/`, with an
additional cross-check in `bootstrap-94121ca-audit.json`. The prepared
guest-built kernel SHA-256 is
`f5d582312e45ab4c3b8fd17534ecd717d1c98ef078b821222c01b2f32d623344`;
the clean counterpart is
`77b2938858889b96077e0bdd4e95cef09e2ef4512345d8a36b1c1ae52837370c`.
The verifier matched each hash to its booted kernel. Writable regular-file
shared mappings, browser/editor usability, recording and showcase acceptance
remain separate requirements.


### Matching acceptance after page-table ownership fixes (2026-10-15)

Both full paths passed at `bf9f82abbbd09e99c8610277ec99a3a67b469dc0`. The suite
now has 14 tests, including `test_vmm_leaf`, which compiles the actual VMM code
with platform/PMM shims and checks concurrent publication, detachment and CoW
retirement ownership. Each path passed all 14 tests twice, booted its extracted
kernel/EFI/modules, and passed all 14 again. All four VMs stopped with checked
synchronization and all twelve final filesystem checks returned zero. Both used
eight vCPUs, 4 GiB RAM, isolated disk-backed images, software graphics and silent
audio. The clean image installed its SDK through guest `nix-install`.

Prepared kernel compilation took 40 seconds, native-test compilation 9 seconds,
and warm native rebuilding 9 seconds. The clean SDK install took 46 seconds;
its kernel build took 167 seconds while a separate 16-thread host stress test
competed for CPU. That host test was suspended, then resumed unchanged after
the VM finished and passed. The loaded clean run is valid correctness evidence
but unsuitable for performance comparison. Run host contention suites separately
from VM performance experiments. Profiling export timed out in both runs;
completed stage/test evidence does not depend on that capture.

Evidence is under ignored
`build/experiments/results/bootstrap-{prepared,clean}-bf9f82a-disk/`, with
`bootstrap-bf9f82a-audit.json` checking suite counts and filesystem results.
The prepared guest-built kernel SHA-256 is
`f8bfaff23797512bb26b3ee5fcaa3c54ab11c307aa493f30ea6028ee7de3cc17`;
the clean counterpart is
`a3e7011396c052af13b7d1abaa889353ad0ee7d255b5e76beb46dbf53d6a582c`.
The verifier matched each hash to its booted kernel.

A subsequent ancestor-permission correction has separate targeted guest
acceptance: the corrected kernel booted a clone of the prepared verification
disks, compiled its expanded VMM test inside Brook, and passed all 14 tests,
CoW/wait stress, heap and filesystem checks. See [workload evidence](workloads.md#page-table-ownership-prerequisite-for-shared-files-15-october-2026).
The full two-path results above remain tied to `bf9f82a`.


### Source archive overwrite correction

The ordinary-file exclusive-create fix documented in `docs/workloads.md`
corrects a fresh-build hazard when overlaying source archives. GNU tar first
tries O_CREAT|O_EXCL and relies on EEXIST to select overwrite behavior. Brook
previously let that open succeed on an existing file, retaining stale trailing
source text when the replacement was shorter. Default GNU tar overwrite now
passes on ext2. Continue checking extracted source hashes before compiling.
FAT timestamp/mode restoration is separate and still requires explicit tar
metadata options; source/bootstrap workspaces should remain on ext2.


### Full refresh after AS lifetime and exclusive creation (3c0ea82)

Both prepared-SDK and clean-image acceptance passed at pinned revision
`3c0ea82ad98d251ba0606a7a8bfdffd97b8d5945`. Each cloned over HTTPS, initialized
submodules, built the kernel/EFI/modules inside Brook, ran all fourteen native
tests, rebuilt and ran all fourteen again, then booted its extracted kernel and
matching modules and passed the suite a third time. The clean run installed
the SDK through guest nix-install. All four VM sessions stopped with checked
flushes; all twelve final read-only FAT/ext2 checks returned zero.

| Guest-reported stage | Prepared SDK | Clean image |
| --- | ---: | ---: |
| SDK install | Already installed | 62 s |
| Kernel build | 48 s | 53 s |
| Native test build | 11 s | 13 s |
| Warm native test rebuild | 12 s | 13 s |
| No-change kernel build | 2 s | 2 s |

These are single profiled runs with an uncontrolled host page cache, not isolated
performance benchmarks. No host contention suite ran alongside either VM.
The previous clean run's 167-second compile was contaminated by concurrent
host stress and must not be used as a speedup baseline.

Prepared guest kernel SHA-256:
`96af84c79e6d7a2ed32a1f7ba96771570d189873c004563f045b089a48cf9b1e`.
Clean guest kernel SHA-256:
`29a88739f0f77e51a720349f33bd63132bab46bc1932fd282f097f1014b56f0f`.
Build IDs/timestamps prevent an assumption of bit-identical binaries.
The audit independently checked actual suite counts, exported/booted kernel
hashes, terminal session state and filesystem results. Evidence is retained in
`build/experiments/results/bootstrap-{prepared,clean}-3c0ea82-disk/`,
`bootstrap-3c0ea82-audit.json` and `bootstrap-3c0ea82-perf.json`.


### Expanded-suite bootstrap acceptance (2026-10-16)

Pinned revision `507e504aa7c848555f3b6946fa63545386c7ed96` passes the prepared
path with eight vCPUs, 4 GiB and RAM-backed disks. The guest built kernel, EFI,
12 modules and seven standalone binaries, passed all 18 native tests twice,
and completed the incremental build. Extracted artifacts then booted independently
and passed all 18 tests a third time in 22.4 host seconds. Every initial and
verification offline filesystem check returned zero; final images were archived
to disk and temporary RAM images removed.

The original observer still expected 14 tests and rejected the larger successful
report. Correction `1d95d71` requires 18 reports and its five observer unit tests
pass; the supported resume path finalized the already-completed build without
rerunning it. The original VM's debug TCP channel rejected shutdown connections.
Its script had already completed its final successful `sync` before EXIT 0, so
QEMU was quit gracefully using that evidence; this was not a fresh bridge sync
acknowledgement. `transport-shutdown-note.json` records the distinction. The
verification VM acknowledged normal checked bridge sync before shutdown.

Compile took 207 guest seconds (207.6 host), native compile 52 seconds, warm native
compile 50 seconds, native suites 20/19 seconds and incremental build four seconds.
An earlier `3c0ea82` disk-backed run recorded 48/11/12/10/5/2 seconds respectively.
Code, backend and host-cache differences prevent an isolated comparison, but the
slower build merits further profiling before making these tests routine. Samples
and source review identify file-page work inside IF-disabled page faults; a scoped
user-fault interruptibility experiment is being validated separately.

The matching clean-image path also passes at the same pinned revision. It proved
the SDK absent before installing it through `nix-install` (277 guest seconds),
then completed kernel compile (207 seconds), native compile (53), both 18-test
suites (18/19), warm rebuild (52), and incremental rebuild (four). Extracted guest
artifacts booted independently and passed all 18 tests in 23.2 host seconds. All
initial and verification filesystem checks returned zero. The original run
required the same documented post-final-sync transport shutdown recovery; its
verification VM acknowledged a normal checked bridge flush. Both completed
results archive their images to disk and remove temporary RAM images.

Evidence: `build/experiments/results/bootstrap-{prepared,clean}-507e504-ram/result.json`,
artifact hashes, stage results, original/verification serial logs and fsck reports.

### Isolated user-page-fault interruptibility experiment (2026-10-16)

Two native clean/rebuild/test rounds used identical prepared `507e504` images,
guest source and SDK, eight vCPUs, 4 GiB and private host RAM disks. The baseline
kernel (`737efba7`) rebuilt in 52.31/50.70 host seconds; the scoped user-fault
IRQ candidate (`5969f234`) took 63.28/62.08 seconds. Suites passed all 18 tests
in both rounds (baseline 18.72/15.81 seconds, candidate 16.74/18.90). Both VMs
acknowledged checked device flushes and passed all three offline filesystem
checks. No bridge/profile queries ran during timed stages; host page cache was
not controlled. The candidate is held out of main: these runs establish no
throughput improvement. Its separate safety gates passed 44 actual mapping
races with SIGSEGV address checks, both-disk coherence and filesystem-exit
checks, 32 futex-exit rounds, 32 mixed-wait exit rounds and fatal fault reaping.

Evidence: `build/workloads/pf-irq-native-comparison-20261016.json`, the two
`pf-irq-native-{baseline,candidate}-20261016/result.json` files and
`pf-irq-races-20261016/safety-results.json`. The unapplied experiment remains
in `build/workloads/user-pf-irq-experiment.patch`; it is not a production fix.

### File-page index experiment and control (2026-10-16)

A separate best-effort growing hash kept inline buckets for small files,
rehashing canonical page nodes as residency increased. Its actual-module test
loaded 4,096 pages through eight concurrent callers, measured 12,189 chain
visits over one complete lookup sweep, and passed allocation-failure recovery,
ordinary-I/O overlays, failed-truncate invalidation and exact reference recovery.
The combined candidate with the CR2 address correction passed the real-guest
race/lifecycle gates described in [workloads.md](workloads.md).

That did not translate into faster native rebuilds: the combined candidate took
71.94/70.97 seconds (18-test suites 19.64/15.80). An original-kernel control
afterward took 54.26 seconds (suite 14.49), close to the earlier baseline. Host
telemetry during that control recorded a 100°C CPU peak and 179 additional
package throttle events; these shared-host conditions limit comparisons but
do not establish the cause of the source-dependent difference. The index is
held out of main pending an isolated CR2-only control and CPU/context-switch
profiles. It must not be presented as an end-to-end performance improvement.

Raw evidence: `build/workloads/file-index-native-comparison-20261016.json`,
`native-baseline-control-20261016/host-telemetry.json`, and the unapplied
`growing-file-index-experiment.patch` beside those result directories.

The subsequent isolated CR2-only kernel (`f9d01a13`) rebuilt in 53.87 seconds
and passed all 18 native tests in 18.82 seconds, followed by checked flush and
zero-error filesystem checks. That matches the original-kernel control closely;
the growing index remains an unapplied experiment. Evidence:
`build/workloads/fault-address-native-20261016/result.json`.

### Recover compiler profiler call chains (2026-10-16)

The previous frame walker confused kernel code addresses with readable stack
storage. Heap-allocated process stacks lie below the kernel image, so compiler
context-switch captures often stopped at DoSwitch. The walker now validates
complete aligned frames within one owned outgoing-task or permanent CPU stack
range, and checks return PCs separately against kernel text or the conservative
module arena. It rejects backward/cyclic/overflowing chains and skips the one
return slot already recorded at context-switch capture. It does not deduplicate
legitimate recursive PCs or change scheduling policy. The new hosted test uses
the actual shared walker; the bootstrap harness now expects 19 native tests.

A live 30-second capture during the controlled native rebuild recorded 12829
CPU samples and 638037 context switches, with 3186577 dropped records. All
recorded switch chains now contain at least two frames. The largest complete
chain (210732 records) blocks in KMutexLock from FileObjectGetPage during mapping
fault resolution. Another 84792 records reach the mapping gate set from fault
resolution, and 43502 reach it from user mapping access. These are counts of
recorded events, not fractions of elapsed time or an unbiased population.
Timer sampling misses IRQ-masked work; recording itself performs FAT writes.
The older apparent dominance of bare DoSwitch/interrupt frames was insufficient
evidence for an IPI-storm diagnosis.

The pinned `507e504` native source rebuilt in 54.91 seconds and passed its 18 tests
in 17.60 seconds during this instrumented run. This verifies the walker on real
compiler stacks, not a speed improvement or full 19-test bootstrap acceptance.
The VM stopped with checked flush; all three offline filesystem checks passed.
Matching frozen symbols, raw/Speedscope profiles, register snapshots and the
stack aggregation are retained under the ignored directories
`build/workloads/debug-tcp-final-production-20261016/artifacts` and
`build/workloads/native-fixed-walker-profile-20261016`.

### Matching 19-test acceptance with the repaired bridge (2026-10-16)

Both RAM-backed paths passed at `0059ce4295a517425ea9faff7c7aa802c20c5c10`,
with eight vCPUs, 4 GiB guest RAM, software graphics and silent audio. Each path
cloned that pushed revision, fetched the two required submodules and built the
kernel, EFI loader, 12 modules and seven bare-metal tests. Each passed all 19
native tests, cleaned/rebuilt them and passed again. Each then booted its own
extracted kernel/modules and passed all 19 tests a third time plus Git fsck.

| Stage, guest seconds | Prepared SDK | Clean image |
| --- | ---: | ---: |
| SDK install via nix-install | Already installed | 277 |
| Clone | 26 | 31 |
| Full compilation | 197 | 201 |
| Native compilation | 53 | 53 |
| Native tests | 21 | 19 |
| Warm native compilation | 53 | 54 |
| Warm native tests | 18 | 16 |
| Incremental build | 5 | 5 |

Host-observed test time under each extracted kernel was 22.13 and 21.46 seconds.
These acceptance runs collect short diagnostic profiles around expensive
stages; they are not uninstrumented optimization benchmarks. Stage metadata
retains both guest markers and host observer times, which can differ when
profile export temporarily delays observation.

All four VM shutdowns acknowledged a fresh checked filesystem/device flush.
FAT and both ext2 disks passed offline checking after each build and each
verification boot. This supersedes the earlier bootstrap runs that needed
manual recovery after losing the debug connection. Evidence, exact artifact
hashes, profiles and stage results remain in
`build/experiments/results/bootstrap-prepared-0059ce4-ram/` and
`build/experiments/results/bootstrap-clean-0059ce4-ram/`. Successful private RAM
images were archived as sparse files and the owned tmpfs directories removed.

### Batch cached file-fault neighbors under one gate (2026-10-16)

A file fault previously called FileObjectGetPage separately for its primary
page and up to 15 cache-only neighbors, handing the same shared inode mutex
off repeatedly among compiler processes. The new FileObjectGetPages operation
loads only the primary page and pins sparse resident neighbors under one gate
acquisition. Each returned frame reference stays owned by a local cleanup guard
until staged into the existing mapping transaction. File/mapping generation
revalidation and retirement ordering remain unchanged. There is no hash-index,
IRQ-policy or read-only AS-check optimization in this change.

The actual file-object module test checks sparse neighbors, eight competing
batches with canonical references, primary-only I/O and one gate acquisition,
heap/frame OOM, partial-read errors, EOF/overflow and pinned frames surviving
failed-truncate cache eviction. All 19 hosted tests passed. The isolated VM
passed 176 fault-race cases and both-disk coherence/lifecycle checks, private
framebuffer aliases, wait cancellation, fatal signal reaping and heap integrity.
Checked shutdown and all three offline filesystem checks passed.

Matched unprofiled rebuilds use the same pinned `507e504` guest source, seed,
eight vCPUs, 4 GiB RAM and private host RAM disks. The unchanged kernel took
52.36/51.78 host seconds. The batching candidate took 30.92/27.45 seconds, then
30.25/27.21 seconds when repeated after the control with its same private
framebuffer prelude: about 42–47% less rebuild time in the matched repeat.
CTest runtime stayed around 17–20 seconds. All rounds passed their 18-test
pinned suite, checked shutdown and offline filesystem checks. Host cache and
thermal state remain uncontrolled; both variants recorded thermal throttling.

A separate instrumented capture recorded file-page mutex-wait chains falling
from 210741 to 7072, with broader mapping-gate waits now prominent. These counts
are supporting diagnostic evidence, not a wall-time percentage: capture windows
cover different fractions of the faster build, and dropped events changed from
3186577 to 1594760. The speed claim uses unprofiled host elapsed time above.
Evidence: `build/workloads/file-page-batch-comparison-20261016.json`,
`build/workloads/file-page-batch-races-20261016/audit.json` and
`build/workloads/native-page-batch-profile-20261016/stack-analysis.json`.
Full bootstrap acceptance above is at `0059ce4`; the new source still needs its
own matching full acceptance paths.

### Avoid file gates for read-only PTE checks (2026-10-16)

UserMappingPageAccess previously acquired every mapped file's gate before
reading one VMA/PTE, even for an already-present anonymous page or an immediate
permission denial. It now performs CheckPage under AS mutation alone. The
existing transaction still pins AS lifetime and protects cancellation. A CoW
candidate releases the read lock, prepares retirement storage, acquires the
original full file-gate transaction and rechecks permissions before copying.
No file metadata is read by the AS-only check; lock order and writable slow-path
retirement remain unchanged.

The actual VMM test holds an unrelated mapped file gate and requires
Ready/Missing/Denied checks to complete without heap allocation. It releases
and joins its worker before asserting, so the previous implementation fails
boundedly rather than hanging. A second test pauses before CoW's full file-gate
acquisition, removes write permission concurrently and verifies that the write
is denied without copying or losing the pinned frame. Existing CoW success,
shootdown/retirement and lifetime tests remain active. The old path failed the
new gate-independence test; the changed path passed all 19 hosted tests.

The changed VM passed 176 fault-race cases, both-disk coherence and filesystem
exit, framebuffer aliases, wait cancellation, fatal signal reaping and heap
integrity. Checked shutdown and all offline filesystem checks passed. On the
same pinned 18-test benchmark source, adding AS-only checks to the batching
baseline reduced rebuilds from 30.25/27.21 to 25.13/22.72 host seconds. Native
tests fell from 19.55/18.48 to 5.28/5.11 seconds. Both candidate rounds passed,
then acknowledged checked shutdown and passed filesystem checks. Host cache and
thermal state remain uncontrolled, and this is a local workload measurement.

Evidence: `build/workloads/as-read-check-negative.log`,
`build/workloads/as-read-check-build.log`,
`build/workloads/as-read-check-races-20261016/audit.json` and
`build/workloads/as-read-check-comparison-20261016.json`. Matching full bootstrap
acceptance must still be rerun at the resulting source revision.

### Matching acceptance after both locking improvements (2026-10-16)

Both full paths passed at `5a363c637f7f5f12a85a22ece8100e743370f823` with the
same SDK, seed types, eight vCPUs, 4 GiB RAM and RAM-backed payload. Each passed
all 19 native tests twice, then booted its own extracted kernel/modules and
passed all 19 again plus Git fsck. All four shutdowns acknowledged fresh checked
filesystem/device flushes; every offline FAT/ext2 check passed.

| Stage, guest seconds | Prepared before | Prepared after | Clean before | Clean after |
| --- | ---: | ---: | ---: | ---: |
| SDK install | Installed | Installed | 277 | 260 |
| Full compilation | 197 | 69 | 201 | 67 |
| Native compilation | 53 | 22 | 53 | 21 |
| Native tests | 21 | 6 | 19 | 6 |
| Warm native compilation | 53 | 20 | 54 | 21 |
| Warm native tests | 18 | 6 | 16 | 7 |
| Incremental build | 5 | 3 | 5 | 3 |

Host-observed verification tests under the extracted kernels took 9.11 and
8.20 seconds. The larger compile improvement is consistent across these two
paths; installation improved much less and remains a separate target. These
acceptance runs collect short profiles; use the unprofiled isolated comparisons
above for the individual optimization measurements. Cache/thermal state is
uncontrolled. Evidence: `build/experiments/results/bootstrap-prepared-5a363c6-ram/`
and `build/experiments/results/bootstrap-clean-5a363c6-ram/`.

The short eight-worker I/O acceptance also passed on this kernel in 22.39 host
seconds, including both VM boots, mixed append/unlink/truncate, boundary and
fragmentation checks, 512 small files, 64 MiB sequential writes/reads and reboot
verification, eight-process random read/mmap integrity, inode lifetime and
signal-mask/select probes. Cleanup restored exact initial free block/inode
counts; checked shutdowns and all offline filesystem checks passed. Guest-clock
phase timings were 0.117 seconds for the 64-MiB write, 0.303 for the read and
0.501 for small files. This is host-RAM-backed I/O, not physical SSD throughput.
Evidence: `build/io-bench/accepted-5a363c6-ram/result.json`.


### Command completion receipts (2026-10-18)

The acceptance harness now launches build and verification scripts as structured
VM bridge jobs. It persists each request identity, stdout/stderr byte cursors
and observed output in `command-evidence.json`. An ambiguous upload observation
reuses the same request. Output gaps, failed exits, missing required markers,
unread output or remaining lineage tasks prevent acceptance; a printed success
marker alone is insufficient. Serial logs remain the source for kernel/device
fault auditing. Observer unit tests cover cursor recovery, ambiguous submission,
output loss and the distinction between leader exit and drained descendants.

These SDK installation/build tests deliberately run as root on isolated disks;
they do not establish unprivileged compilation or package-install permissions.
The matching 38-suite acceptance below uses this command protocol.


### Matching 38-suite acceptance (2026-10-18)

Both full paths passed at `049d22e6c95db146ed68088843f37f55c5dba0ac`,
including the filesystem capacity fix, Firefox audio integration, signal/entry
changes and current locking changes. Each eight-vCPU, 4-GiB run cloned that
remote revision and both required submodules, built the EFI loader, kernel,
12 modules and seven bare-metal test ELFs, and passed all 38 native suites.
Each then cleaned/rebuilt the native tests and passed all 38 again. Independent
verification disks booted each guest-built kernel (checked against its extracted
hash), passed Git fsck and ran all 38 suites a third time. The seven bare-metal
ELFs were built and hashed; this does not claim their execution.

| Stage, guest seconds | Prepared SDK | Clean image |
| --- | ---: | ---: |
| SDK installation via nix-install | Already installed | 732 |
| Clone | 14 | 14 |
| Checkout | 5 | 8 |
| Submodules | 20 | 19 |
| Full compilation | 51 | 51 |
| Native compilation | 29 | 30 |
| Native tests | 8 | 24 |
| Warm native compilation | 29 | 27 |
| Warm native tests | 9 | 9 |
| Incremental build | 2 | 3 |

The clean run's first native-test outlier is `test_fd_table`: 15.51 seconds,
versus 0.67 seconds in the prepared run's first pass. Its 500-round concurrent
close stress and pin/use/close tests still pass; this variance needs a targeted
scheduling/latency investigation before drawing a performance conclusion.
Host-observed verification, including command/output observation, took 15.00
and 18.92 seconds. These are instrumented correctness runs, not a controlled
before/after benchmark. Several CPU/context-switch captures report producer
drops, and IRQ-masked work is not sampled uniformly.

Both use the pinned `wryc8rh1a9rb7gr528fdhzp9n38rjwda-brook-bootstrap` SDK.
The starting fixtures are `bootstrap-prepared-parity-20261014` (guest-NAR SDK
modes) and `bootstrap-clean-20261014`, rather than newly generated desktop
images. The clean guest checks that its store contains only the base closure
and no compiler/SDK shell, then installs 91 additions from a host-local binary
cache through its seeded installer. This verifies the current kernel and
source against those fixtures; it does not validate the latest Gallery UI or
replace fresh-image acceptance for current package-tool changes. Both commands
run as root; unprivileged bootstrap remains outside this result.

Installation remains expensive with host-RAM-backed images. The 502,507,680-byte
LLVM library NAR took 152,584 ms to extract, and the 817,440,144-byte Clang library
NAR took 166,013 ms. Curl, decompressor and unpacker run concurrently; their
similar pipeline durations are not additive and do not identify the limiting
stage. The two install samples mostly contain idle CPUs. Next isolate raw
transfer, local decompression, pipes and local filesystem writes using the same
payload before choosing a kernel or installer optimization. The older 260-second
install result above is not renewed performance acceptance for this revision.

All four command jobs exited zero with zero live lineage tasks and both output
streams at EOF. All four VMs acknowledged checked sync, exited zero and passed
the serial audit; FAT and both ext2 disks passed every offline check. Their
owned tmpfs payloads were saved as sparse regular files and removed from RAM.
Evidence and exact artifact hashes:

- `build/experiments/results/bootstrap-prepared-049d22e-ram/`
- `build/experiments/results/bootstrap-clean-049d22e-ram/`
- `build/workloads/bootstrap-049d22e-acceptance-audit-20261018.json`

Each run retains `command-evidence.json` for the build and verification,
`result.json`, profile paths, per-stage block counters and host-environment
metadata. The clean run also retains a separate mid-install profile. This
acceptance closes the matching full-bootstrap rerun gap for the pinned revision;
application co-use, real-site persistence and broader desktop regressions remain
separate work.


### Matching 39-suite acceptance after TX ownership (2026-10-18)

Both full paths pass at `610592de4f5463f8d5a453844b07d7f6e1c30614` after
[serializing virtio-net TX ownership](package-pipeline.md). The new actual-driver
fixture brings the suite to 39 tests. Each path cloned that exact remote source
and its required submodules, built the kernel/EFI loader, 12 modules and seven
bare-metal test ELFs, and passed all 39 native suites twice, including a clean
native rebuild. Independent disks then booted each guest-built kernel, verified
its hash, passed Git fsck and ran all 39 suites a third time. The staged ESP EFI
and kernel match the extracted hashes; the verification data disk's virtio-net
module also matches the guest-built artifact. Bare-metal ELFs were built/hashed,
not executed by this acceptance.

| Stage, guest seconds | Prepared SDK | Clean image |
| --- | ---: | ---: |
| SDK installation via nix-install | Already installed | 236 |
| Clone | 14 | 14 |
| Checkout | 7 | 7 |
| Submodules | 19 | 20 |
| Full compilation | 51 | 50 |
| Native compilation | 30 | 30 |
| Native tests | 9 | 9 |
| Warm native compilation | 29 | 28 |
| Warm native tests | 10 | 9 |
| Incremental build | 1 | 2 |

The same clean seed and pinned SDK took 732 seconds to install in the preceding
38-suite run. The new 236-second result is about 68% lower; observer marker
intervals likewise fell from 729.67 to 234.77 host seconds. The initial kernel
ELF is byte-identical and the virtio-net module differs. The controlled component
comparisons and negative driver test provide additional evidence for the fix;
these full acceptance runs still have uncontrolled host cache/thermal state and
short diagnostic profiles. No browser-rendering speedup or general disk
throughput percentage follows from these numbers.

Within the installer, the LLVM library's 502,507,680-byte NAR fell from 152,584 to
51,625 ms; the Clang library's 817,440,144-byte NAR fell from 166,013 to 46,588 ms.
All 91 SDK additions registered. The starting prepared/clean fixtures, host-local
binary cache, SDK identity and root execution scope are the same as documented
in the preceding acceptance. This does not replace fresh-image/current Gallery
installation acceptance. Compiling remains around 50 seconds. Verification
under the guest-built kernels took 16.58 host seconds for prepared and 15.68 for
clean, including command/output observation.

All four build/verification jobs exited zero with zero live lineage tasks and
both streams at EOF. All four VMs acknowledged checked sync, exited zero and
passed serial auditing, including the new TX timeout/bad-completion checks.
Every offline FAT/ext2 check passed. Successful owned RAM images were archived
and their tmpfs directories released. Evidence:

- `build/experiments/results/bootstrap-clean-610592d-ram/`
- `build/experiments/results/bootstrap-prepared-610592d-ram/`
- `build/workloads/bootstrap-610592d-acceptance-audit-20261018.json`

This supersedes the pending full-rerun note for the TX change and renews both
bootstrap paths with the current 39-suite source. Real application co-use,
profile/cookie persistence, permissions and remaining desktop work still need
their own acceptance.

### Matching 39-suite refresh after clock and desktop changes (2026-10-18)

Both full paths pass at `f4066d0c78c3196d07344789d1cdce774c7fb0ef`, renewing
acceptance after the realtime/block completion clock and desktop session-service
changes. Each isolated eight-vCPU/4-GiB VM cloned that exact remote revision,
built kernel/EFI, 12 modules, seven bare-metal test ELFs and 39 native suites,
and passed the native suites before and after a clean native rebuild. Independent
verification disks then booted the respective guest-built kernels, passed Git
fsck and all 39 suites again. Bare-metal ELFs were built and hashed, not executed.
The bootstrap runs use root maintenance identity, not a non-root build acceptance.

| Stage, guest seconds | Prepared SDK | Clean image |
| --- | ---: | ---: |
| SDK installation via nix-install | Already installed | 239 |
| Clone | 16 | 14 |
| Checkout | 6 | 8 |
| Submodules | 19 | 19 |
| Full compilation | 51 | 53 |
| Native compilation | 31 | 30 |
| Native tests | 8 | 8 |
| Warm native compilation | 30 | 30 |
| Warm native tests | 8 | 8 |
| Incremental build | 3 | 3 |

The same clean seed and pinned SDK previously installed in 236 seconds. The
239-second refresh and 51–53-second compilation are baseline consistency checks,
not evidence of a new speedup. Host cache/thermal state and observer overhead
remain uncontrolled. The largest clean-run NARs took 51,181 ms for the
502,507,680-byte LLVM library and 46,772 ms for the 817,440,144-byte Clang library.
Those are end-to-end pipeline intervals on RAM-backed disks, not physical disk
throughput measurements or isolated decompressor CPU times.

Twenty coarse clean-run snapshots observed up to 646.5 MiB used physical memory;
nine prepared snapshots observed up to 746.9 MiB, both during native compilation.
These approximately 20-second samples include kernel/cache memory and miss
unobserved peaks; they are not per-process RSS or a memory improvement claim.
Three-second diagnostic profiles lost records in clone, checkout, compilation
and native tests. Their loss counters are retained; they cannot establish
unbiased CPU percentages or a new bottleneck. Shorter targeted captures are
needed before attributing remaining package/compilation costs.

All four tracked commands exited zero, with no remaining tagged descendants
and both output streams complete. All four launchers exited zero after checked
sync/stop; all FAT/ext2 checks passed. The 60 extracted outputs per path match
their recorded hashes, staged ESP kernel/EFI files and deployed verification
modules. The launcher's temporary EFI disk was deleted on exit; the independent
post-run comparison covers the retained ESP staging, not that removed raw disk.
Successful RAM images were archived and their owned tmpfs directories released.

The clean guest-built kernel SHA-256 is
`43f995297dd4164e168421df939e14e5443b821b4ecc560b107f59f904735988`;
prepared is
`dc22eba01ca3a0630703e480f07999f0637a086501c9cde695dac0d504764e48`.
Each verification launch used its own extracted output; cross-build byte
reproducibility is not claimed.

A blanket user-fault rejection is incorrect for this suite: `test_user_copy`
intentionally faults real REP instructions and tests signal-return register
preservation. The new [fault auditor](../scripts/audit_bootstrap_faults.py)
checks visible records against the extracted test's exact instruction/fixup and
handler symbols, operand direction, signal delivery and completed test receipt.
An exact large-RAX sentinel case uses completed in-test assertions because the
kernel suppresses its restore-register log for RAX above 512. Unrelated images,
instructions, handlers, incomplete receipts and unproven recovery fail closed;
seven host regression tests exercise these rejection paths. The bootstrap
harness now invokes this audit before publishing success. That host-side hook
was added after the live runs and verified against all four completed records.

The clean build/verification logs contain 9/5 intact fault banners; prepared
contains 10/5. Serial interleaving corrupts some additional probe text, so this
is a classification of visible records, not a lossless exception census. The
collected native test results remain the evidence for their assertions. This
does not extend coverage to every interrupt vector or every raw user-pointer
access path. No kernel/device panic or unexpected intact fault record was found.

Evidence is under ignored:

- `build/experiments/results/bootstrap-clean-f4066d0-ram/`
- `build/experiments/results/bootstrap-prepared-f4066d0-ram/`
- `build/workloads/bootstrap-f4066d0-20261018/acceptance-audit.json`

Both result directories contain the separate `user-fault-audit.json`. This
supersedes the pending full-rerun notes for the clock/session kernel changes;
application, identity, Gallery and desktop acceptance still have their own scope.


### 40-suite bootstrap refresh after permission checks (2026-10-18)

Both paths passed at pinned remote revision
`37cccf6087e96b26e2e0224fadabacb2dbc421e6`, including the shared component walker
and real/effective-ID access inquiries. The acceptance harness now requires
**40** native suites; its stale 39-suite expectation was updated, with checks
that incomplete or old-count output is rejected. Each path built and hashed
61 artifacts: kernel/EFI, twelve modules, seven bare-metal test ELFs and forty
native test binaries. The bare-metal ELFs were built, not executed. All native
suites passed before and after a clean rebuild, then a third time under that
path's exact guest-built kernel. The verification ESP's EFI/kernel and all
staged modules match the extracted outputs; all twelve modules have ABI 18.

| Stage (guest wall seconds) | Prepared SDK | Clean image |
| --- | ---: | ---: |
| SDK install through nix-install | Already installed | 245 |
| HTTPS clone | 15 | 14 |
| Pinned checkout | 7 | 7 |
| Submodules | 19 | 20 |
| Kernel/module/bare-metal build | 52 | 54 |
| Native configure | 4 | 4 |
| Native compile | 30 | 31 |
| Native tests | 9 | 8 |
| Warm native compile | 30 | 29 |
| Warm native tests | 9 | 8 |
| Incremental kernel build | 2 | 2 |

These eight-vCPU, 4-GiB guests used private RAM-backed disks. Both paths
completed checked sync/stop, zero QEMU/launcher exit, terminal original/full
command lineages, both EOFs without gaps/drops, Git fsck and both sets of three
read-only filesystem checks. Owned RAM images were preserved on disk and freed.
The original guests booted kernel
`50fe3e571183c246225af8cbe724133394f8bdd3ce71a5afd7dbcdf997be368e`.
The independent guest-built verification kernels were:

- Prepared: `9578d4dcbb221dcffa5a678589d552985049c923335e3c91605b7d0133cb1580`.
- Clean: `8dbdcbb6ba8321386290b1c076760dfb19ad1cda57322a82b1b5d78f6a5b6aa3`.

Cross-build byte reproducibility is not claimed. Each original/verification
serial pair contained 10/5 intact user-fault banners, all classified as the
pinned, recovered user-copy probes by the existing fault auditor. This remains
best-effort serial evidence, not expanded interrupt or raw-pointer coverage.

The clean run exposed a host-side profiling failure: compiler output moved the
profile filename outside the bridge's 256-KiB recent-log window. The guest kept
building. Its original session/request was resumed without redispatch, and all
acceptance gates completed. The first recovery verification launcher lacked
the Nix shell's firmware environment and exited before QEMU started; the same
staged outputs/disks were then verified inside nix-shell. Those diagnostics and
recovery notes are retained; a fresh uninterrupted run of the host-side fixes
has not been performed.

The bridge now searches backwards with bounded memory for the latest complete
profile filename within the current boot's serial range. It rejects partial
filenames and prior-boot references. Forty-seven bridge contract tests passed,
including long compiler output and every split position in a filename record.
Offline recovery from the actual long build log also found and decoded the
completed profile. A missing optional profile reference now records a diagnostic
and disables further automatic captures instead of abandoning the build monitor;
kernel-identity mismatches and other validation errors still fail acceptance.

The three-second stage profiles remain partly lossy and cannot establish a
reliable bottleneck ranking. A separate 500-ms install capture retained 396 CPU
samples and 7,525 context switches with zero drops and matching event counts.
389 of those CPU samples landed in idle loops; IRQ-masked time may be
underrepresented, and one window is not a stage-wide estimate. User/module
addresses were not attributed to the nearest kernel symbol. The concurrent
process census showed the curl/zstd/nar-unpack pipeline. Installer-reported
unpack wall times included 53 seconds for an 817-MB Clang library NAR, 50 seconds
for a 503-MB LLVM NAR and 36 seconds for a 277-MB GCC NAR. These are pipelined
wall times, not isolated disk bandwidth. RAM-backed storage and substantial
idle sampling motivate separating decompression, pipes, filesystem writes and
wakeups in a focused experiment before selecting an optimization. The earlier
239-second SDK install and 51–53-second compile remain comparable single-run
baselines; host load/cache and this run's extra short profile prevent a precise
performance-change claim.

Local ignored evidence:

- `build/experiments/results/bootstrap-prepared-37cccf60-ram/`
- `build/experiments/results/bootstrap-clean-37cccf60-ram/`
- `build/workloads/bootstrap-37cccf60-20261018/acceptance-audit.json`
- `build/workloads/bootstrap-37cccf60-20261018/install-short-profile.json`
- `build/workloads/bootstrap-37cccf60-20261018/package-timings.json`
- `build/workloads/bootstrap-37cccf60-20261018/profile-lookup-recovery.json`

This renews bootstrap acceptance for the permission source revision, including
the SDK install on the clean image. Real-application concurrency, complete
identity isolation and App Gallery interruption/disk-full recovery still have
separate outstanding acceptance work.

## 44-suite bootstrap refresh (18 October)

Prepared and clean images both built pinned remote revision
`ead5537988f4b8b53982e258e335d1d1c6428353`. Fresh seeds were recreated from current
package-tool sources with the same SDK
`/nix/store/wryc8rh1a9rb7gr528fdhzp9n38rjwda-brook-bootstrap`.
The clean image verified its base-only store and absent SDK before installing
through guest `nix-install`. The prepared SDK uses guest-NAR-equivalent modes.
Neither seed contains Quake data. These are root bootstrap commands, not
unprivileged self-hosting or a complete desktop permission boundary.

Each eight-vCPU/4-GiB VM used private host-tmpfs disks, software virtio graphics,
the kernel compositor without Wayland apps, silent audio and 4 GiB host reserve.
Both paths performed HTTPS clone, pinned checkout/submodules, kernel/EFI/module/
bare-metal builds, 44 native tests, clean native rebuild and another 44-test
pass. Independent VMs booted each path's guest-built kernel and passed all 44
again. Bare-metal test ELFs were built, not executed.

| Stage (guest wall seconds) | Prepared SDK | Clean image |
| --- | ---: | ---: |
| SDK installation | Already installed | 243 |
| HTTPS clone | 13 | 14 |
| Checkout | 8 | 7 |
| Submodules | 20 | 20 |
| Kernel/module/bare-metal compilation | 52 | 53 |
| Native configuration | 4 | 4 |
| Native compilation | 31 | 31 |
| Native tests | 8 | 9 |
| Warm native compilation | 31 | 31 |
| Warm native tests | 8 | 9 |
| Incremental kernel build | 3 | 2 |

These are instrumented single runs with one-second stage resolution and
uncontrolled host cache/load. The previous 245-second install and 52–54-second
kernel builds remain observations, not evidence of a speedup. Periodically
observed guest used memory peaked around 735 MiB prepared and 699 MiB clean;
these snapshots are not allocation high-water marks.

Each path produced 65 hashed artifacts: kernel/EFI, twelve modules, seven
bare-metal ELFs and 44 native binaries. Independent `.modinfo` inspection found
ABI 20 throughout. Verification ESP/kernel/module bytes matched extracted
outputs. Guest kernel SHA-256:

- Prepared: `613a36b043a61315277e4a16fab46d0a3111975b9bb47280e41ce3c8523f6b83`.
- Clean: `906e92470031d8daf215fd095857bb5e93035144c3c2443d638072fcc67119ec`.

Both original outer Nix-shell/harness invocations returned zero. All four QEMU
identities were independently dead with exit zero. Primary tracked commands
had terminal exit zero, original/full lineages empty and exact stdout/stderr EOF
without gaps, drops or read errors. Checked sync and both sets of three read-only
FAT/data/Nix checks passed per path. Images were saved as sparse regular files
and owned tmpfs directories removed. The historical harness does not separately
persist the inner `run_experiment.sh` wrapper's return status; QEMU and outer
harness evidence must not be relabelled as that missing receipt. Visible user
faults were recognised deliberate recovery probes in the exact built user-copy
ELF; serial interleaving prevents an exhaustive fault-count claim.

### Performance evidence

`scripts/bootstrap_perf_report.py` now pairs explicit block-counter tables
around build stages. It subtracts monotonic counters and retains cumulative
maxima without subtracting them; missing/reset/device-change data is explicit.
Six parser tests pass. Counters include whole-VM/profiling work, aggregate waits
can overlap, and requests do not measure bytes. Installation has no automatic
before/after tables, so whole-stage disk attribution remains unavailable.

Many three-second build captures overflowed. The initial clean-install capture
retained 2,390 CPU samples without drops, including 2,358 idle leaves. A later
500-ms capture retained 394 CPU samples and 7,675 switches without drops, with
358 idle leaves. IRQ-masked work is underrepresented and these intervals do not
represent the full install.

A separate ten-second host interval measured about 4.41 CPU-seconds across
selected QEMU threads (0.44 cores on average). vCPUs 0/1 used around 12%/19% of a
core. Guest block counters around that interval recorded about 0.266 aggregate
completion-wait seconds and 6,958 requests on the Nix disk, with no new flush
errors. This motivates separating pipeline pacing, network delivery,
decompression and filesystem work; it does not identify one bottleneck or
establish physical disk latency as the dominant cost.

The owned `tools/io-bench/pipecheck.c` control transfers and byte-verifies 8 MiB
through one/two pipes with requested 4-KiB/64-KiB chunks, verifies producer reports
and waits every child. The same static binary passed on Linux and in a separate
eight-vCPU/4-GiB Brook VM as UID 1000. Two guest rounds took roughly 25–77 ms per
case; Linux took roughly 4 ms. Guest 64-KiB requests still required about 2,000
writes per pipe, consistent with the current 4-KiB syscall cap. Timings include
process lifecycle and byte checking, without decompression, file output,
metadata or networking. This throughput alone does not explain SDK install
time. Next compare controlled NAR extraction with and without those stages,
preserving exact output hashes. This is not general pipe cancellation or
concurrent-reader verification.

The control VM drained all user address spaces/finalizers, passed heap integrity,
used orderly MCP shutdown and exited zero; three read-only checks were clean.
Local ignored evidence:

- `build/experiments/results/bootstrap-{prepared,clean}-ead55379-ram/`.
- `build/workloads/bootstrap-ead55379-20261018/acceptance-audit.json`.
- `build/bootstrap-ead55379-perf-20261018.json`.
- Clean-run `install-short-profile.json` and `install-host-interval/`.
- `build/workloads/pipe-control-20261018/review/` and
  `build/pipecheck-linux-20261018.log`.
