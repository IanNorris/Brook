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

The shared-library startup and native compatibility gate now passes all 13
tests. The filesystem gate also passes the expanded guest lifetime probe and
offline fsck after a clean shutdown. Both the prepared-SDK and clean-image
paths passed the full sequence at the same revision, `8042310`.

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
It validates module imports and requires 13/13 CTest results twice, before and
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
already exited zero with both 13-test summaries and artifact markers. It does
not turn a failed or hung guest build into acceptance. Shutdown evidence records
whether sync was acknowledged live or came from an already completed script
and a normal launcher-recorded exit.

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
command replies, and reruns Git fsck and all 13 native tests under that kernel,
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
