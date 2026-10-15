# Real application acceptance

Use one isolated VM at a time, disk-backed clones and silent host audio. Keep
runtime disks, package archives, private logs and raw snapshots under ignored
`build/`. Screenshots come from the VM rather than the host desktop. After a
run, require checked guest synchronization, stop the VM and check FAT and both
ext2 filesystems offline without repair.

## Compilers

The bootstrap SDK includes the wrapped native compiler in `HOST_CC` and
`HOST_CXX`, alongside separate freestanding kernel compilers. Run
`scripts/workloads/cpp.sh` inside Brook with the SDK installed. It compiles a
C++17 program that sorts 10,000 integers in a worker thread, joins, checks their
sum and writes/reopens its result through standard C++ file streams. Success
requires both `CPP_WORKLOAD_PASS total=50005000` and `CPP_SCRIPT_EXIT_0`.

For Rust, export the stock compiler from the same package pin:

```sh
nix-shell --run 'python3 scripts/export_guest_package.py --expression tools/workload-pkg/rustc.nix --name brook-rustc --version 1.94.1'
```

Serve the ignored package cache on the isolated test VM's host-loopback cache
server, add `brook-rustc.idx` to the guest package index, then install through
`NIX_CACHE_URL=http://10.0.2.2:PORT /nix/bin/nix-install brook-rustc`.
Run `scripts/workloads/rust.sh` inside Brook. It exercises Rust code generation,
linking, a joined worker thread, vector sorting, assertions and filesystem I/O.
Require both `RUST_WORKLOAD_PASS total=50005000` and `RUST_SCRIPT_EXIT_0`.
The script explicitly selects the SDK's `HOST_CC` as Rust's linker. In the first
run, default `cc` lookup selected `/boot/BIN/cc`, whose legacy `tcc` target was
absent. Plain default-linker Rust compilation is therefore not established.

On 2026-10-15 both programs passed in an eight-vCPU, 2 GiB Brook VM with
kernel SHA-256
`a461247abfe6da520f30ee44aa13dc7bbda34d9e16b35eb92d9023588d83fee9`.
Rust 1.94.1 was installed through guest `nix-install`, including its roughly
1 GiB compiler payload. After checked shutdown, FAT and both ext2 images
passed offline checks. Results, serial output and script hashes are retained
under `build/workloads/vim-20261015/`. These focused programs do not establish
Cargo, arbitrary C++ projects or compiler self-hosting.

## Terminal editors

The package expression `tools/workload-pkg/vim.nix` selects stock Vim from the
same pin. The current package is 9.2.0340; the older shipped search index names
a different version, so record the actual exported store path and version.

Use the actual Brook terminal window and VM keyboard input, initially with
ASCII text. Launch:

```sh
/nix/profile/bin/vim -Nu NONE -i NONE /data/editor-acceptance.txt
```

Keep Vim's normal swap and backup behavior. Verify exact bytes after typing,
saving and quitting. Reopen, replace the contents with shorter text, save and
verify there is no old trailing data. Reopen for a native-resolution screenshot,
then quit and verify shell input/echo and process cleanup. An Ex-mode script
alone does not establish interactive terminal behavior.

The first run started Vim but reported `E1286: Could not set timeout: Function
not implemented`; serial output identified missing syscall 222 (`timer_create`).
Dismissing recurring warnings allowed editing, but the first long input lost
characters and failed exact-content acceptance. A shorter replacement saved
exactly `Short.\n`, with SHA-256
`05e7af01711bc1fd49c9bb2f92dd1b9550e7e2d3183701d0056dbf1955efee5e`,
and the shell accepted `echo SHELL_RETURN_OK` after quitting.
That failed run is retained as the baseline. The subsequent timer fix and
interactive acceptance are recorded below.

Brook's current kernel terminal uses pipes and TTY ioctls. Full Linux PTY
support (`/dev/ptmx`, `/dev/pts`, allocation/unlock, controlling terminal and
hangup semantics) remains separate work. Helix and Emacs have not been tested
in this acceptance sequence. Codium, Blender and Firefox/Chromium also remain
outstanding.


## Vim timer fix and interactive result

The candidate kernel
`5c1b05f58cc50439c3b0d6c7504dd39fdcd27426ed4866620c215082cb74017d`
passed the focused ASCII editor acceptance on 2026-10-15. Vim started without
E1286, saved the exact three-line, 132-byte input, reopened it visibly, then
replaced it with exactly `Short.\n`. The long file's SHA-256 was
`f5ea421c1caf7a3441c09f665bfd1eba5e6134c0454a03fe3c26ad640300d182`;
the shorter file matched the hash above. The terminal accepted
`echo VIM_SHELL_RETURN_OK` after quitting, and the guest returned to its
nine-process baseline. No unused window-input queue overflow warnings appeared.

[Native 1920×1080 Vim screenshot](images/brook-vim-1080.png)

Brook now provides bounded POSIX timers for realtime/monotonic clocks and
signal, no-notification and Linux thread-target notifications. Glibc implements
Vim's thread callbacks using a targeted internal signal; preserving SI_TIMER
and its opaque value through synchronous signal waits is required. Merely
accepting timer_create would not run those callbacks. Timer IDs and recipient
incarnation checks prevent stale IDs or recycled TIDs from selecting another
recipient. Timers are removed on exec/group exit and final thread retirement.
The old exit wrapper also treated a leader's SYS_exit as exit_group; removing
that coercion allows surviving siblings to keep using their process's timers.

`tools/at-syscall-probe/posixtimercheck.c` passed on Linux/glibc and twice inside
Brook, including a second run after Vim. Each guest run observed target-TID
reuse, recovered all 128 timer slots, and executed 16 actual glibc callbacks.
The probe also covers signal metadata, independent timers sharing a signal,
overruns, disarm/rearm, ignored signals, invalid arguments, fork/exec/exit
cleanup and leader-thread exit with surviving siblings. See the probe README
for ABI details and Linux reference-test quirks.

All 13 host tests and seven bare-metal tests passed. C++ and Rust workloads
passed again on this candidate. Checked shutdown and
offline FAT/ext2 checks all passed. Evidence, hashes and the original screenshot
are under `build/workloads/vim-timer-fixed-20261015/`.

This establishes the tested ASCII editing workflow, rather than full terminal
emulation or PTY support. CPU-only asynchronous handler delivery at interrupt
return and timer-driven job-control signals remain unsupported; those timer
notifications return an explicit error. The probe README records these limits.


## Codium compatibility investigation (15 October 2026)

Stock VSCodium 1.112.01907 installs through the guest package installer. Its
193 MiB ELF exposed silent 32 MiB executable-image truncation in both shell
launch and execve. The shared reader now reads the exact image with bounded
512 MiB allocation and explicit errors, and the loader copies page-sized chunks
rather than translating every byte. A 40 MiB file-backed payload passes both
launch paths. Seven malformed ELF variants return ENOEXEC while a sibling
heartbeat thread continues, before destructive exec replacement.

Chromium's thread discovery also requires `/proc/self/task` and its directory
link count. The new task-directory regression verifies live TIDs, single-thread
link count, thread-self/status, foreign-thread rejection, and a held directory
across 32 exit/reuse cycles. The guest observed a reused PID without retargeting
the stale directory. The general stale-dirfd path-resolution case remains a
separate followup; this test covers held vnode enumeration/stat.

Codium then rejected its shared memory because independent FAT opens reported
different inode numbers. FAT now reports device plus physical directory-entry
identity, including cached files and stat-by-path; anonymous memfd backings get
unique stable identities shared by dup/fork. `fileidentitycheck` passes on the
boot FAT `/tmp` and ext2 `/data`, with 4 KiB and cached 128 KiB files, repeated
stat/fstat/lstat/fstatat, truncate/reopen, dup/fork and distinct memfds. FAT rename
can relocate its directory entry, and unlink/recreate can reuse it: persistent
POSIX identity across those operations is not claimed.

The latest guest kernel for the identity check is
`35f26dcd952025262a49a954f39c82d047d2bde6fb5457eb2030fd5650cb68a0`.
The shared-memory inode mismatch and earlier thread-discovery fatal are gone.
Codium still hits a user-mode Chromium thread breakpoint before showing an
editor. A successful package install or CLI launcher exit is not a working
editor acceptance result. Inotify and netlink support are also still missing.

A subsequent software-rendered run with the Wayland server active, kernel
`8729d7ee097d225dbc093f775bdacfe21104ea5b20d70b87adc731db1de5d7fe`,
confirmed a writable regular-file `MAP_SHARED` rejection for 262,144 bytes
at offset zero (`prot=3`, `flags=1`, `EOPNOTSUPP`). The following breakpoint
matches Chromium's `FieldTrialAllocator` startup path. The temporary diagnostic
was removed after collecting this evidence in bridge session
`7a03c37188334fae92a8fa8eccfa4c4c`. A separate hardware-rendered run also
encountered an earlier roughly 2 MiB Mesa shared-file mapping rejection.

At that point regular-file `ftruncate` and `truncate` were success-only stubs.
Shared file pages, ordinary I/O coherence and dirty writeback also require
actual filesystem and mapping support, rather than making a private copy and
reporting successful shared mapping. The `/dev/shm` synthetic
namespace additionally needs proper entry references and unlink behavior before
it can provide a reliable alternative shared-memory path. These remain open
compatibility work; neither Codium nor Chromium is accepted yet.

## Firefox compatibility investigation (15 October 2026)

Stock Firefox 150.0.1 exports through `tools/workload-pkg/firefox.nix` and
installs successfully through guest `nix-install` as `firefox-stock`. This
establishes package extraction and registration, not a working browser.
The experiment uses a separate desktop disk clone, software graphics, silent
audio and a local HTML/JavaScript input page. Match the application's UID to
its home-directory owner; Firefox correctly refuses a root launch with a
regular user's home directory.

The first wrapped launch failed XPCOM discovery. `/proc/self/exe` retained the
unresolved executable symlink spelling. Launching the underlying executable
directly reached Wayland and a later startup assertion. A candidate that records
the resolved executable target also gets the normal installed wrapper past
XPCOM discovery. Its executable-symlink probe passed three cases and a direct
shell launch, but a valid short shebang script exposed a separate minimum-ELF-size
check before interpreter processing. Full candidate acceptance remains pending.

The later crash is an intentional `SharedMap::MaybeRebuild` assertion, not an
unresolved arbitrary fault: the exact installed `libxul.so` instruction maps
to `MOZ_RELEASE_ASSERT(!buffer.error())` in the matching release source.
Firefox's Linux read-only memfd duplication opens `/proc/self/fd/N`.
Brook instead supplies a regular procfs file containing printable link text.
A guest reproducer wrote an 18-byte marker to a memfd, reopened it through
procfs, and read six bytes, `memfd\n`. Correct capability reopening and access
rights are required; passing a private copy or the link text is not sharing.

The first two diagnostic VMs stopped with checked synchronization and clean
FAT/data/Nix offline checks. A subsequent ABI 9 candidate passed all four
executable-path cases, memfd procfd access/mapping checks and the mapping-capacity
gate. Reopening the minimal marker now returned the exact 18 bytes. With the
upstream `MOZ_SHM_NO_SEALS=1` option, the installed Firefox wrapper progressed
past SharedMap initialization, then exposed a kernel socket lifetime fault.
A live, pinned Unix socket descriptor referred to an allocation already reused
for font configuration text. The fault occurred in `PollWait`; this candidate
is not accepted. Descriptor duplication and fork contained publication/retain
races sufficient to cause premature release, though this run's exact historical
release was not uniquely established.

The panicked VM required forced unsynchronized shutdown. Read-only offline
checks returned boot 0, data 4 and Nix 0; these images are retained without
repair and must not seed further tests. Evidence, exact kernels, serial logs,
the reproducer and crash-reporter screenshot remain under ignored
`build/workloads/firefox-20261015/` and its bridge sessions. Subsequent socket
ownership tests use fresh clones from the clean desktop base. The crash reporter
renders with native Brook chrome; no browser window or interactive webpage has
passed acceptance yet.

The next socket-ownership candidate passed the concurrent socket duplicate/close
and fork probe, executable-path tests, both procfd modes, and filesystem exit
and truncate gates on FAT/ext2. Two Firefox launches progressed through proxy
socket setup without a kernel panic, but each faulted writing a new cursor
shared-memory mapping. The guest stayed responsive and its heap integrity check
passed; checked shutdown and all three offline filesystem checks passed too.
These runs do not establish general pending-connection or SCM_RIGHTS queue
concurrency correctness.

A full trace showed successful `fallocate(fd, 0, 0, ...)` immediately before
`mmap(..., MAP_SHARED, fd, 0)`. Brook's success-only fallocate stub had left the
memfd at zero bytes. A separate minimal program confirmed this: Linux returned
success and size 2,304, while Brook returned success and size zero. The next
required fix is actual preallocation and logical growth, with unsupported
backends/modes returning errors. Mapping must not silently extend the file to
compensate. Evidence is under ignored
`build/workloads/firefox-unix-20261015/`; Firefox remains unaccepted.

The real memfd preallocation candidate passed `FALLOCHECK --brook`, including
allocated-block checks before first touch and `KEEP_SIZE` reservation beyond EOF.
The prior FAT/ext2 fixtures verified after reboot, and executable/procfd/socket
regressions passed again. Firefox progressed past the cursor mapping and spawned
its worker threads, then intentionally aborted in
`ChannelPosix::SetPipe` (`ipc_channel_posix.cc:128`). The next success-only stub
was `getsockopt(SOL_SOCKET, SO_SNDBUF)`: it returned success without writing a
positive buffer size. The kernel remained responsive. Evidence and matching
release source are under ignored `build/workloads/firefox-fallocate-20261015/`;
a browser window or interactive webpage is still unverified.

The next candidate passed the socket-option probe, including a nonblocking
write test that filled the connected pipe to its reported 65,535-byte capacity
and then required `EAGAIN`. Unsupported options now return errors. This exposed
a required option in the Wayland server: tracing `waylandd` showed
`getsockopt(SOL_SOCKET, SO_PEERCRED)` returning `ENOPROTOOPT`, immediately followed
by removal and closure of the accepted connection. Firefox's proxy then reported
a compositor failure even though the server remained alive. Correct peer
credential snapshots are required next. The socket-option candidate passed
filesystem exit/truncate checks and heap integrity, stopped with checked sync,
and passed all three offline filesystem checks. Evidence is under ignored
`build/workloads/firefox-socketopt-20261015/`.

The peer-credential candidate passed the UID-1000 client/root-listener test,
inherited endpoint/listener identity, thread-group identity, and short-output
checks, alongside all previous focused gates. Its Firefox run nevertheless
reproduced the original kernel socket corruption: a live pinned descriptor
again referenced an allocation overwritten with font configuration data before
listener setup. The earlier descriptor ownership fixes therefore do not establish
that this intermittent UAF is resolved. The candidate is not accepted. Forced
unsynchronized shutdown was required; read-only checks returned boot 0, data 4
and Nix 0, and the images are retained without repair under ignored
`build/workloads/firefox-peercred-20261015/`. The next diagnostic needs socket
allocation/reference history and heap-free overlap records in a bounded RAM ring,
so the release can be attributed rather than inferred from the corrupted bytes.


The bounded lifetime trace identified the release: a client closed before its
server-side accept, while the pending slot held ancillary queue pointers without
owning references. Accept then reused those freed queues, eventually freeing a
new listener allocation. The pending slot now owns both server pipe endpoints
and queue references, transfers them only after successful descriptor publication,
and releases abandoned connections on listener teardown. Connect completes once
queued instead of retaining a raw client waiter through accept. Pipe endpoint
release is serialized with its final-owner decision.

Kernel `0972c8dc7038344bfaa0b863cf9a0f4a0dfefea375ec81fecb1fa76b7c52c0d7`
passed the deterministic 32-round early-close/SCM_RIGHTS probe, listener
abandonment, accept `EMFILE` retry, previous compatibility probes, and FAT/ext2
exit/truncate gates. Heap integrity, checked shutdown and all three read-only
filesystem checks passed. Evidence is under ignored
`build/workloads/firefox-pending-20261015/`. This does not establish general
ancillary queue concurrency correctness or full Unix socket semantics.

Firefox then initialized Wayland and progressed to its WebAssembly runtime,
which aborted because `ARCH_SET_GS` was missing. The next candidate adds real
per-thread user GS state, preserves the CPU-local kernel GS, rejects bases
outside the canonical user half, and resets user GS on exec. The Linux and Brook
GS probe passed four-thread isolation with 8,000 yields, actual GS loads,
inheritance and exec reset; the 13 hosted tests and previous focused guest gates
also passed. Its Firefox retry drew the browser tabs, address bar and toolbar, then stalled
with blank page content. Paused CPU frames show a user-destination write fault
inside an ext2 cache-hit copy while the cache lock remains held. Other CPUs
waiting for that lock with interrupts disabled cannot acknowledge a pending TLB
shootdown, forming a wait cycle. The paused state and 1080p screenshot are
retained; no usable browser or interactive page is accepted. Candidate evidence is under ignored
`build/workloads/firefox-gs-20261015/`. The ext2 candidate now stages user reads through a bounded 64 KiB kernel
buffer and releases filesystem/cache locks before copying out. Kernel-buffer
reads retain direct coalesced I/O. Missing cache buffers are allocated outside
the cache lock, rechecked before publication and freed if another reader won.
The initial cache/CoW probe reproduced the old kernel's wait cycle; the expanded
probe passed kernel
`b46c826ecb44389e83556c1bc22ce735796ea8423e5d53feb0785945c35eff6e`.
It checks eight rounds of four children with four threads each, concurrent file
faults, exact bytes, multi-chunk reads, EOF offsets and parent CoW isolation.
The initial and expanded Linux reference probes also passed. Evidence is under
ignored `build/workloads/cachecow-baseline-20261015/` and
`build/workloads/firefox-cache-20261015/`. The 13 focused compatibility gates and FAT/ext2 exit/truncate gates passed,
followed by heap integrity, checked shutdown and all three offline filesystem
checks. After reboot, persisted fixtures and the focused gates passed again.
Both full bootstrap paths subsequently passed at `e5dd9aa`, including repeated
13-test suites, guest-built-kernel boots and all offline filesystem checks. See
[bootstrap acceptance](bootstrap.md#matching-acceptance-after-browser-compatibility-fixes-2026-10-15).
The Firefox retry remains under investigation.

## Child waiting and stop/resume compatibility (15 October 2026)

The candidate kernel implements x86-64 `waitid` with `P_ALL`, `P_PID` and
`P_PGID`, including nonblocking observation, `WNOWAIT`, stopped/continued events,
actual terminating signals and the 128-byte Linux siginfo layout. `wait4` uses
the same atomic child-event snapshot and consumption path. Child ownership is
recorded by parent thread group, so one thread can wait for another thread's
child even after the creating thread exits. Consuming waiters and the automatic
reaper detach a child under the process-registry lock before destruction outside
locks; concurrent waiters cannot both claim it.

The stop/continue stress exposed two scheduler races: a pending block could
replace `Stopped` with `Blocked`, and SIGCONT could enqueue a child whose stack
was still owned by another CPU. Stopped tasks now retain their state through
ordinary wakeups. An independent deferred-resume flag authorizes publication
only after CPU ownership clears; consuming a continued event does not consume
that scheduling permission. New stops cancel deferred resumption.

The candidate kernel SHA-256 is
`a12dd9ad285d8514beeceddb1c7756c3556539047def736f4713aa651f680c0b`.
`waitidcheck` passed six runs of 32 competing-waiter races and 32 stop/continue
cycles, followed by three expanded runs covering pipe/ignored-signal wakeups
while stopped, delayed continuation and repeated SIGCONT. Other cases cover
exit status 200, repeated peeks, invalid selectors/flags/pointers, signal death,
thread-group parent survival, process groups, a killed blocked waiter and
`EINTR`. The final expanded probe also passes on Linux. All 13 host tests,
13 guest compatibility gates, cache/CoW and FAT/ext2 exit/resize stress passed.
Heap integrity, checked shutdown and all three read-only filesystem checks
passed. Evidence is retained under ignored
`build/workloads/firefox-waitid-resume-20261015/`.

This is not full Linux job-control or wait compatibility: PIDFD selection and
extended `__W*` options return `EINVAL`, and usage reports currently tracked
leader CPU ticks rather than full thread-group accounting. Both full bootstrap
paths subsequently passed at `94121ca`, including a third test suite under
each guest-built kernel and all twelve final filesystem checks. See the
[matching acceptance](bootstrap.md#matching-acceptance-after-atomic-child-waits-2026-10-15). Firefox's
first retry no longer emits the earlier child-wait error-38 warnings, but page
content remains blank and child channels still fail; a usable browser is not
yet accepted. A bounded errors-only trace also captured rejection of a
2,097,160-byte writable shared mapping (`prot=3`, `flags=1`, `EOPNOTSUPP`).
The trace does not identify that file descriptor's backing object or establish
this as the cause of every content-process failure. Disabling the fork server
via an environment-variable experiment did not resolve blank content; whether
that build honored the variable was not established. Browser evidence is kept
under `build/workloads/firefox-waitid-browser-20261015/`.

## Page-table ownership prerequisite for shared files (15 October 2026)

Fault publication now uses a non-overwriting leaf operation. Two concurrent
faults cannot both install their candidate frames at the same virtual address;
the loser releases its candidate reference. Both regular-file and memfd faults,
including speculative file-cache installs, use this operation. A separate
atomic detach operation returns the displaced PTE without releasing its frame.
Present 1 GiB and 2 MiB leaves remain untouched by these 4 KiB operations.

CoW replacement now transfers the old PTE reference to its caller rather than
releasing it before the remote TLB acknowledgement. Both user faults and
syscall output-buffer validation retain it through shootdown, then release it.
The validator preserves interrupt state through that short ownership window.

`test_vmm_leaf` compiles the actual VMM implementation with host physical frames
and interrupt/invalidation shims. It checks 64 competing install/detach rounds,
32 concurrent CoW rounds, allocation failure, in-place CoW, large-leaf rejection,
borrowed device-frame ownership and same-address replacement while the old
retirement is delayed. Its acknowledgement barrier proves software reference
ownership, not hardware IPI behavior. All 14 host tests pass. The bootstrap
harness now requires all 14 tests and artifacts; the previous 13-test bootstrap
results above remain evidence for their pinned revisions.

The reviewed guest kernel SHA-256 is
`2f5b403db4f8a92cae87ca1a1f002ddadb4eb96fefd1d16ac7a6d94a5cdd140b`.
Guest compatibility checks, cache/CoW stress, FAT/ext2 exit/resize stress,
three wait probes (including two expanded runs), heap integrity, checked stop
and all three read-only filesystem checks pass. Evidence is retained under
ignored `build/workloads/mapping-leaf-reviewed-20261015/`.

Both full bootstrap paths subsequently passed at `bf9f82a`, including all 14
host tests twice and again under each guest-built kernel, with all twelve final
filesystem checks clean. A followup makes occupied and failed installs preserve
ancestor user-access permissions as well as the leaf. Its expanded test fails
on the earlier implementation and passes the corrected code. The corrected
kernel SHA-256 is
`191d60cf1ee437841fa5291dee61473b59e263b80d1cdff975211cec886c404b`.
An isolated clone of the clean prepared verification disks compiled the expanded
test inside Brook and passed all 14 tests, cache/CoW and expanded wait stress,
heap integrity, checked stop and all three filesystem checks. That targeted
followup is not a second clean-image clone/install acceptance. Evidence is under
`build/workloads/mapping-ancestor-20261015/`.

This is the leaf-ownership layer of the mapping plan. Address-space mutation
serialization and generation validation across blocking fault I/O remain open,
as do coherent shared pages, buffered I/O visibility, dirty writeback/msync and
resize invalidation. Regular-file writable `MAP_SHARED` still returns
`EOPNOTSUPP`; neither Codium nor Firefox is accepted as a usable workload.

## File resizing prerequisite

The subsequent FAT/ext2 resize implementation replaces those success-only
stubs. FAT growth writes zeroes and updates independently opened file handles,
including previously cached readers. Ext2 growth is sparse; shrink detaches
direct/indirect blocks before making allocations reusable and clears the
retained partial block. File offsets remain unchanged. Invalid descriptors,
negative lengths and read-only descriptors return errors.

Live lazy file mappings currently make resize fail with `EBUSY`, including
`O_TRUNC`. The registry retains backing references during faults; the guest
probe verifies serialized rejection and recovery after unmap. Full concurrent
fault/unmap/mprotect/MAP_FIXED ordering still needs a coordinated mapping
lifecycle change, and cross-process resize invalidation remains unimplemented.
FAT rename/unlink also rejects open file handles because its directory-slot identities cannot
otherwise preserve those handles safely. Ext4 resize returns `EOPNOTSUPP`.
These are explicit compatibility limits, not full Linux mmap/resize semantics.
The appended VFS operation requires module ABI 7 and rebuilt driver modules.

`truncatecheck` passed inside Brook on boot FAT and 4 KiB-block ext2, covering
shrink/regrow zeroes, independent handles, cached reads, unchanged offsets,
direct/single/double-indirect boundary markers, mapped resize rejection,
protection splits and implicit fork-child exit cleanup. Both retained fixtures
passed verification after reboot. The final tested kernel was
`baf79343e5503a8b7f7442fe2e9b28b476206b5e33631178529d49fdeaad2c72`;
checked shutdown and offline FAT/data/nix checks passed. Evidence is retained
under `build/workloads/icons-theme-20261015/truncate-cancel-acceptance.json`.
Faults now publish their transient backing ownership in the current thread;
normal return and off-CPU teardown atomically release the same reference/pin.
Sixteen kill/fault-loop rounds passed per filesystem without leaving the file
busy. This timing stress does not prove cancellation at a particular blocked
I/O instruction. The 1/2 KiB ext2 matrix remains a separate gate, as does the
concurrent mapping lifecycle work above. This does not establish writable file
mappings or successful Codium startup.


## Address-space ownership and cleanup (15 October 2026)

Threads now retain a separate address-space object containing the page table,
TLB footprint, mapping metadata and allocation cursors. Fork creates independent
state; exec replaces the task's reference. Final reclamation runs on a cleanup
worker rather than in scheduler interrupt context. Consuming wait calls wait
for the worker's finite cleanup batch, preserving immediate kill/wait/truncate
behavior. Nonleader exec snapshots its own descriptor table before detaching.

Native windows retain the original AS for buffer unmap after exec. Legacy
compositor buffers have explicit AS ownership and enter compositor epoch
retirement only after their leaves are removed. Forked graphics leaves retain
managed child references without COW-downgrading borrowed parent leaves.
The guest `src/apps/as_lifetimecheck.c` probe exercises legacy framebuffer
resize, native window resize, inherited graphics backing and old-window
cleanup after exec over a newly mapped address.

Candidate kernel SHA-256
`d1ca7e9d0b0f05dc4d622bb1168d31a72c77e6038ef588b8a4a84805fcce3b2e`
passed all 14 hosted tests on the host and after rebuilding inside Brook,
cache/CoW, expanded waitid, the graphics probe, five filesystem mutation gates
and thirteen compatibility/persistence gates in an isolated eight-vCPU,
4 GiB VM. Heap integrity was OK. Checked shutdown and all three offline
filesystem checks passed. Evidence is retained under
`build/workloads/address-space-wait-20261015/`. Source overlays were checked
against an exact hash manifest before compiling. Use `brook-bootstrap-shell`
for native compiler linking; plain Bash lacks its libc++ linker flags.

This establishes the lifetime layer, not a common mapping mutation protocol.
The mutex/generation fields are preparatory. Fault snapshots, anonymous/device
replacement, protection changes, source-window reference acquisition and shared
file writeback still need integration. General device-fork coherence and Linux
nonleader-exec semantics are not established. Codium and Firefox acceptance
remain incomplete; writable regular-file MAP_SHARED is still unsupported.

A separate tar overwrite reproduction found ordinary-file O_CREAT|O_EXCL is
ignored: GNU tar's initial exclusive open succeeds on an existing file, so it
never switches to its truncate/overwrite path. A six-byte entry left a 46-byte
file. `--unlink-first` plus source hashes worked around this for the focused
source-overlay validation; exclusive creation needs a separate filesystem fix.


## Atomic exclusive creation and GNU tar overwrite (15 October 2026)

Ordinary-file O_CREAT|O_EXCL now rejects existing directory entries with EEXIST,
without following a final symlink or truncating content. The existing namespace
mutation mutex covers the existence check through creation, and regular creates,
mkdir and symlink now share that gate with rename/unlink. The checked open API
also preserves truncate errors and errors from the legacy /boot alias; the
original VfsOpenMode entry point remains available to existing callers.

`tools/at-syscall-probe/exclusivecheck.c` passed sixteen rounds of eight
simultaneous creators on host Linux, then in Brook on ext2 and FAT: exactly one
creator succeeded per round and seven received EEXIST. Existing file/directory
preservation and O_EXCL without O_CREAT passed. Ext2 additionally passed dangling
and live final symlinks plus creation through a symlinked parent; FAT exercised
the boot-path alias. FAT symlink behavior was not part of this probe.

Kernel SHA-256
`f78e54af993762e17111615600d00fe17efb41e4dee75c50265259bc8b600cb5`
passed the new probe on both filesystems, all fourteen host tests, five guest
I/O mutation gates and thirteen guest compatibility/persistence gates. Heap
integrity was OK; checked shutdown and all three offline filesystem checks
passed. Evidence: `build/workloads/exclusive-create-20261015/`.

Actual GNU tar replaced the 46-byte fixture with exactly `short\n` (six bytes),
SHA-256 `c962fa1be311981f0f965857e89b000707f9cea07a069d073461308f3019200f`.
Default GNU tar passed on ext2. FAT required explicit
`--touch --no-same-permissions --no-same-owner`: default extraction now overwrites
the bytes correctly but reports unsupported timestamp/mode restoration. The
successful final two-filesystem tar run took about half a second; the first
harness wait included diagnosis/retry and is not a performance measurement.
Generic backend errors and boolean path-resolution failures remain existing
limitations beyond this targeted exclusive-create fix.


## Prepared mapping transactions and heap updates (15 October 2026)

`MappingTransaction` now retains the executing task's address space and a kernel
region across preparation, commit and retirement. It allocates records and
zeroes candidate frames before taking the AS mutation mutex. Detached leaves
and backing callbacks remain owned until the mutex is released and remote TLB
acknowledgements complete. Rollback conditionally detaches only the matching
published physical frame, and is idempotent within the original lock epoch.
Physical matching alone is not an incarnation guard against legacy writers.

`brk` is the first consumer. Heap bounds live with the address space, including
fork/exec initialization. Growth prepares zeroed pages before publication and
rolls back partially published pages on allocation failure. Shrink retains
removed frames through shootdown instead of releasing them first. Generation
validation retries preparation after another participating writer changes the
AS. Other mapping writers and faults have not yet joined this protocol; lazy
mapping overlap and range reservations still need the common range engine.
This does not enable regular-file MAP_SHARED or establish browser acceptance.

Actual transaction/VMM host tests cover delayed acknowledgement, unlocked frame
and backing retirement, zero-before-publication, occupied/replaced leaves,
idempotent rollback, record/frame allocation failures and partial publication
rollback. Scheduler cancellation is instrumented with host shims; deterministic
real cancellation tests remain outstanding.

Candidate kernel SHA-256
`5d8b705f740e1b5f056b50b38931bbfabda6d8e1b56baf97173db02ebf0d9fff`
passed all 14 host tests and all 14 after rebuilding the changed test sources
inside Brook against a checked source manifest. Guest shared-heap growth,
partial/aligned shrink/regrowth, fork isolation, graphics lifetime, expanded
waitid, cache/CoW, five I/O gates and thirteen compatibility/persistence gates
passed. Heap integrity was OK; checked shutdown and all three offline filesystem
checks passed. This focused run used eight vCPUs, 4 GiB guest RAM and sparse host
tmpfs disks; it is not a fresh two-path bootstrap acceptance run. Evidence:
`build/workloads/mapping-transaction-20261015/`.

The new `brkexeccheck` passes on both the accepted baseline and candidate. It
checks Brook's current nonleader-exec detachment model: a new image has its own
heap while the surviving old-AS tasks still share theirs. It is not proof of
Linux nonleader-exec semantics or a baseline regression reproduction. A leader
exec terminates siblings; an earlier test assuming otherwise was discarded.
During candidate CMake regeneration, cache-file rename attempts still logged
failure, although regeneration and the actual newly compiled tests completed.
That remaining overwrite/fallback behavior needs a focused investigation.
