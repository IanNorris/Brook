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


## Common range model and sparse teardown foundation (16 October 2026)

The memory subsystem now has a sorted range model for anonymous reservations,
files, memfds, devices, images and stacks. A range carries identity, backing
offset, current/maximum protections, sharing policy and explicit managed or
borrowed leaf ownership. Descriptor references retain one existing backing pin;
retaining a descriptor cannot enter filesystem code, and final release runs
outside mapping locks. Split/replacement nodes are prepared before locking.
Failed capacity, coverage or permission preflights leave the table unchanged.

Transaction helpers retain immutable fault snapshots, reject changed identity,
coverage, offset or protection, and keep the backing live through retirement.
Ordinary page population no longer increments topology generation, allowing a
validated fault snapshot to cover adjacent pages. Combined range removal checks
metadata capacity and populated-leaf retirement capacity before detaching any
leaves. Sparse range traversal examines allocated page tables, including two
populated pages a terabyte apart, rather than every virtual page. Unsupported
large user leaves cause a preflight failure before any retirement callback.

Address-space finalization now walks populated page tables directly and queries
range ownership, with the legacy metadata/tag fallback during migration. This
also removes the old per-virtual-page teardown loops for sparse memfd/framebuffer
extents. Descriptor-owned borrowed User-tagged leaves survive until their backing
release; managed leaves drop their own references after TLB acknowledgement.

This is a foundation, **not the completed common mapping protocol**. Live mmap,
munmap, mprotect, mremap, file/memfd fault handlers, fork/CoW and GUI writers still
need migration. Their legacy metadata remains authoritative until that switch;
the new table is not populated by those syscalls yet. The deterministic snapshot
tests exercise actual helper code with modeled preparation/ACK barriers, not a
paused real filesystem fault. PROT_NONE content retention, cached/borrowed fault
publication, syscall range semantics and complete concurrent-writer acceptance
remain required. Regular-file MAP_SHARED stays disabled; Codium/Firefox acceptance
is still incomplete. Sparse helper tests do not establish sparse live munmap
performance while the syscall still uses its legacy implementation.

Candidate kernel SHA-256
`7be1659f3a38452a8055b5a6e44afe4884a986adbc1d7fed4fcfcd6ede2d4192`
passed the host suite and all 14 tests after rebuilding the source-hashed changes
inside Brook. New cases cover 1,000 independent page-oracle interval edits,
allocation failure, stale primary/adjacent snapshots, delayed ACK/backing pins,
borrowed User-tagged memfd leaves, high-PML4 teardown, a 1 TiB sparse removal and
mixed ordinary/large-leaf failure atomicity. Existing heap, graphics, cache/CoW,
expanded waitid, five I/O and thirteen compatibility/persistence gates passed.
Heap integrity, checked shutdown and all three offline filesystem checks passed.
Evidence: `build/workloads/mapping-ranges-20261016/audit.json` and adjacent logs.
The final run used eight vCPUs and 4 GiB with disk-backed isolated clones because
available host memory was below the RAM-disk guard. This is focused validation,
not a fresh two-path bootstrap acceptance run or a browser performance result.


## Integrated mapping candidate (16 October 2026, worktree)

Candidate kernel `83cf2e491bf95ffe777e4655ae67b42a8ce23d3cba9671c744d65b0197f71543`
integrates the live range/mutation protocol across syscalls, faults, fork/CoW,
image construction, remote stacks and graphics. Fixed legacy mapping arrays are
removed. This supersedes the earlier foundation-only status for the current
worktree; the implementation remains uncommitted pending broader acceptance.

With eight vCPUs, 4 GiB RAM and isolated disk-backed clones, the candidate booted,
verified a 229-file source manifest, rebuilt the hosted tests inside Brook and
passed all 14 targets. Existing heap/CoW, expanded wait, graphics resize/fork/exec,
five filesystem checks and twelve compatibility checks passed. The thirteenth
check initially assumed the retired 256-mapping limit and deliberately wrote to
memory after an unexpectedly successful PROT_NONE operation. Its updated source
was compiled inside Brook and passed with 768 mappings, protection splits,
expected read/write faults, byte preservation and cleanup. The test harness also
needed an explicit probe-bundle install step for fresh disks.

A new child-with-native-window-and-private-file-map test passed 32 exit/wait/
immediate-truncate rounds on each of FAT and ext2. It did not inject a compositor
stall. Native terminal launch, typed input, shell exit and slot reuse passed;
resize-under-mapping and output exceeding pipe capacity remain untested. VM text
injection produced `#` for a requested backslash, so keyboard-layout agreement
still needs investigation before a live-coding showcase.

Heap integrity passed. The VM stopped with checked filesystem/device flush,
the launcher exited zero, and all three offline filesystem checks returned zero.
Evidence: `build/workloads/live-mapping-20261016/audit.json` and adjacent logs.
This is focused integration validation, not renewed two-path bootstrap acceptance
or Chromium/Codium acceptance. Deterministic real-fault races, cancellation,
delayed-renderer consuming-wait coverage and coherent regular-file shared writes
are still required; regular-file writable MAP_SHARED remains disabled.

## Deterministic guest fault races (16 October 2026, worktree)

The opt-in `BROOK_MAPPING_TEST_HOOKS` kernel runs barriers in the actual fault
resolver after capturing a retained range and after preparing its pages, outside
mapping/backing locks. The guest `mappingracecheck` probe exposed a real failure:
when MAP_FIXED replaced a file range with an anonymous range during preparation,
the file-only retry rejected the new kind and killed a valid reader. Retries now
dispatch from the current mapping kind and accept an already accessible leaf
only after checking the current range permissions. Existing private managed CoW
leaves retry into ordinary CoW handling. Astra reviewed the fix and hook locks.

Candidate `8da11f6ce21034ac763fd406fe43bad2b3f540038321b23fda3c42ced8487ce5`
passed all 22 cases on ext2 and all 22 on FAT, in approximately 0.93 and 0.90
seconds respectively. Cases cover both phases, file and memfd backings,
same/adjacent-page faults, replacement-byte preservation, expected SIGSEGV after
unmap/write-permission removal, and SIGKILL with immediate truncate after
consuming wait. Five I/O and all thirteen compatibility gates passed, with a
rebuilt 768-mapping capacity probe. Cache/CoW, native graphics lifecycle and
expanded wait probes also passed. Both kernel configurations build; the ordinary
configuration excludes the hook syscall and waits. All fourteen hosted tests pass.

The first two instrumentation attempts stalled because the test wait did not
restore interrupts at entry and after yielding; those failed results are retained.
Astra also identified a plain-spinlock preemption risk, corrected with short
IRQ-disabled state sections. These are test-harness corrections, separate from
the reproduced and fixed stale-mapping fault. The passing candidate used fresh
isolated disk clones, eight vCPUs and 4 GiB RAM. Its source manifest was verified,
heap integrity passed, checked shutdown completed, the launcher exited zero and
all three offline filesystem checks returned zero. Evidence:
`build/workloads/mapping-races-fixed-20261016/audit.json` and adjacent logs.

This covers cancellation at preparation boundaries, not inside a blocked disk
request or retirement. The following checkpoint adds delayed-renderer and
terminal resize/large-output evidence. Coherent regular-file MAP_SHARED
writes/writeback, renewed full bootstrap tests and browser/editor acceptance
remain outstanding. No Chromium/Codium success or
performance result is implied by this focused gate.


### Terminal exec, resize and delayed-renderer checkpoint (2026-10-16)

Candidate `4f4e74ddd1c4439e4073d31cd059b39fd0574f84416b536df66228bec11cdfaf`
fixes two terminal lifecycle issues found during acceptance. TIOCGWINSZ now
copies rows, columns and pixel geometry under the terminal state lock, so a
resize cannot expose partially updated geometry. Successful exec of the owning
shell republishes the terminal-owned pixel source after replacing the process
image; previously its window showed wallpaper until a resize. This restoration
matches only the terminal owner, not a forked descendant.

The real native terminal ran `exec /boot/TERMCHK /data/TERMCHK.OK` using VM
keyboard input. The probe verified its original framebuffer immediately after
exec, wrote exactly 262,144 bytes in 379 ms, then accepted two GUI resizes to
77×29 and 74×28 characters. It retained a user alias to the old pixel allocation
through the second resize and three compositor epochs, checked its tail canary,
and verified zero padding in the new allocation before writing a distinct
canary. The source remained visible before either resize. Its watchdog uses a
POSIX timer, and its initial dimensions must differ from the fallback 640×480
framebuffer. This is one software-rendered run, not a terminal throughput study.

The opt-in hook also pauses the compositor at frame entry, before taking scene
or source snapshots. All 32 native-window/file-exit cases on ext2 and all 32 on
FAT completed consuming wait and immediate truncate while the compositor was
still parked. This verifies file cleanup does not depend on advancing renderer
epochs. It does not hold an old pixel snapshot or an in-flight GPU submission.

On the same eight-vCPU, 4 GiB VM, all 44 mapping-race cases, five I/O gates,
thirteen compatibility gates, native graphics lifecycle and expanded wait tests
passed. Fourteen hosted tests passed; both kernel configurations build. The
source manifest matched, heap integrity passed, checked filesystem/device flush
completed, and all three offline filesystem checks returned zero. Evidence:
`build/workloads/terminal-exec-fixed-20261016/audit.json`, adjacent serial logs
and the pre-resize screenshot recorded in `terminal-results.json`. The previous
renderer/terminal candidate retains the reproduced exec-display failure.

Cancellation during blocked disk I/O or retirement, coherent regular-file
MAP_SHARED writes/writeback, both renewed full bootstrap runs and browser/editor
acceptance remain required. These focused checks do not establish those results.


### Canonical page adoption and PMM reference capacity (2026-10-16)

The reusable clean cache now returns the canonical winning frame after concurrent
inserts. Fault preparation stages that frame, and read-ahead releases its caller
reference while leaving the cache reference intact. Private and read-only
mappings retain the existing uncached fallback. This does not enable writable
regular-file MAP_SHARED or add ordinary-I/O coherence. Ext2 regular files and FAT
still opt out of the cache; its convergence is demonstrated by the actual module
hosted test with eight simultaneous separate-open contenders.

Astra's review exposed physical-page references silently saturating at 255. The
real PMM regression reproduced count 255 instead of 256, then passed after
widening references and user-PTE mapping counts to 32 bits. The CoW consumer and
provenance counters are widened too; overflow fails explicitly. The descriptor
array grows by four bytes per physical frame, or 4 MiB at 4 GiB RAM. Reordering
the provenance record preserves its existing 80-byte size. Standalone test
kernels now link an invariant panic handler that reports failure through the
QEMU test exit port. Their build caught this missing dependency.

Candidate `ccf132cf1e6a4941ce103b639ec881ead0bbe26dcaecca34335d9421381a4944`
passes sixteen hosted tests, including actual PMM accounting for 70,000 logical
aliases, 16,384 concurrent pins/releases and final freeing. This fixture uses
real PMM locking, lists and bitmap transitions; it does not execute hardware
page tables or TLB shootdowns. A separate VM probe created 512 real writable
aliases to one framebuffer page, verified their shared canary, closed the fd,
and removed aliases while checking the survivors.

On the same isolated eight-vCPU/4-GiB VM, all 44 mapping race cases, 64 delayed
renderer cleanup cases, five I/O gates, thirteen compatibility gates, native
graphics and expanded wait checks passed. The actual terminal exec/resize probe
passed again (262,144 bytes in 429 ms, old-alias canary preserved after three
epochs). Heap integrity passed; the 373-file kernel/probe/test manifest matched,
checked shutdown completed and all three offline filesystem checks returned
zero. Both kernel configurations and standalone test binaries build. Evidence:
`build/workloads/file-cache-pmm-20261016/audit.json` and adjacent logs.

The Linux-contract file-sharing probe passed host Linux and was then run inside
Brook: the first writable shared mapping fails with errno 95 (ENOTSUP). That is
a recorded failure baseline, not an acceptance pass. Coherent regular-file I/O,
writeback, resize/EOF semantics, both renewed full bootstrap paths and browser
acceptance remain required; see `file-shared-mappings.md`.


### Ext2 live-inode file objects and ordinary I/O coherence (2026-10-16)

Ext2 now shares canonical mapped pages through its retained live-inode holder,
with VnodeOps ABI 13. Separate opens share completed pages without a vnode
reference cycle or cache identity surviving inode reuse. Ordinary I/O passes
through the same per-file gate; successful write prefixes update resident pages,
and reads overlay them onto backend data. User copy happens through bounded
resident staging outside file/filesystem locks. Unmapped resize drops cached
pages only on backend success. Failed or incomplete page loads are never cached.
Writable shared stores, writeback/msync and mapped truncate remain unsupported.

The actual-module hosted test verifies a single backend page load under eight
contending opens, I/O visibility, short-read recovery, premature zero/error with
successful retry, partial write errors, EOF tails and final cleanup. The Linux
reference and actual Brook `filecoherencecheck` both pass. Guest cases cover
separate opens, parallel first touches, read-only alias visibility after pwrite,
private CoW, file-backed source/destination buffers, unmapped truncate/regrow,
and unlink/close lifetime. An additional run uses a previously untouched mapped
source page. Syscall validation populates that source; no fault inside the later
copy is claimed, and the guest barrier is not a deterministic same-miss hook.

Candidate `1860fb1984d7a5b841a7ccc59c2a8ecdceb043bf02dbdb94880fc75174917282`
passes seventeen hosted tests, both kernel configurations and standalone test
builds. The same eight-vCPU/4-GiB VM passes all 44 mapping races, 64 frame-entry
paused renderer cleanup cases, five I/O gates, thirteen compatibility gates and
native graphics/wait lifecycle tests. The source manifests match, heap checks
pass, shutdown completes checked filesystem/device flush and all three offline
filesystem checks return zero. Evidence: `build/workloads/file-object-20261016/audit.json`
and the strengthened probe run `build/workloads/file-object-cold-20261016/audit.json`.
The cold-source run uses the identical kernel hash; focused gates were not
unnecessarily repeated for the probe-only change.

This is the ordinary-I/O portion of coherent file mappings. FAT retains its prior
uncached path. Ordered file gates and per-AS registrations, mapped resize/EOF,
dirty writeback/msync, both renewed full bootstrap paths and browser acceptance
remain required. The design and deterministic acceptance cases are recorded in
`file-shared-mappings.md`.


### File registration checkpoint (2026-10-16)

The ABI-14 candidate in `build/workloads/file-registry-20261016` passes the same
focused coherence, 44 mapping-race, 64 renderer-cleanup, five I/O and thirteen
compatibility gates, plus graphics/wait lifecycle checks. Seventeen hosted tests
also cover weak registrations through split/fork/failed fork, ref-zero teardown,
delayed TLB acknowledgement and allocation failures. All three offline filesystem
checks and heap integrity pass; see its `audit.json` and source manifest.

A bounded native-window probe sends two close requests and survives ten seconds
after the last one, then exits itself. Normal close no longer escalates to forced
termination. This does not yet prove an actual editor's unsaved-document dialog.
Mapped truncate, writable regular-file MAP_SHARED, dirty writeback and both full
bootstrap reruns remain pending.

For the user's desktop test, `build/experiments/results/manual-20261016/run.sh`
uses a frozen production kernel (test hooks off), matching ABI-14 drivers and
clones of the full `apps-ready-20261014` images. All clones passed offline checks.
It selects native SDL graphics, virgl, PipeWire audio, eight CPUs and 4 GiB RAM.
This combined desktop checkpoint has not yet been boot-tested. Do not run builds
or another VM while the user is exploring it.


### User desktop smoke test (2026-10-16, in progress)

The first manual checkpoint accidentally paired the current kernel with the older
`apps-ready-20261014` image. The user observed missing icons and apparently dead
launcher entries, then confirmed GIMP eventually opened after a long delay.
Process/window inspection confirms applications launch; this is not evidence that
all shortcuts fail. LibreOffice logs show a DeploymentException and exit status 1,
plus pending-thread teardown timeouts. Keep this separate from GIMP latency.

The prepared replacement `manual-apps-20261016` uses the later
`icons-theme-20261015` images, matching ABI-14 modules and the same frozen
production kernel. The icon directory contains the expected 2304-byte RGBA
assets and all three offline filesystem checks return zero. It has not yet been
boot-tested, so launcher and application acceptance remain unproven.

A live non-intrusive stats sample from the first session at guest tick 351759 ms
reported 44 processes, roughly 2.04 GB used and 2.21 GB free. Recent 256
compositor-loop intervals were p50 21 ms, p90 39 ms, p95 56 ms and max 3100 ms.
These are guest-loop intervals, not application GPU or scanout frame times, and
neither isolate startup cost nor establish its cause. Preserve the old run for
controlled comparison rather than treating the replacement assets as a fix for
all application failures.


### Corrected desktop verification (2026-10-16)

`build/workloads/desktop-smoke-20261016/audit.json` records the corrected full-app
image with production kernel `96d5fdadead4b39372c5bbf10294f52d61266620ba815cbfd53ae55d791575e6`
(ABI 14, mapping test hooks off). The resize error-path host test passes and the
production kernel builds. In the accelerated eight-vCPU/4-GiB guest, the native
launcher, taskbar and titlebar show icons. Mousepad launched through the actual
menu and displayed the exact two injected lines. Its unsaved-document dialog
survived beyond five seconds and Cancel restored the edited document. Final
normal application exit was not verified; the VM then stopped with checked
filesystem/device flush and all three offline filesystem checks returned zero.

Startup remains slow. A five-second idle-editor profile contains 4071 samples,
7324 context switches and no dropped records. Of 282 non-idle samples, 247 are at
the `virtio_gpu` SubmitCommand polling loop. This establishes a small reproduction
for command-wait overhead; it does not explain all application startup costs.
The earlier multi-app capture and exact-module disassembly are preserved in
`build/experiments/results/manual-20261016/slow-capture/`.

The profiler converter no longer labels module addresses as enormous offsets
into the kernel's last function. Without explicit module symbols those addresses
remain numeric. The saved multi-app capture has a corrected Speedscope export.
Neither the IRQ-disabled allocator sampling bias nor full module symbolization
is solved by that correction.

The local `build/experiments/current-apps` pointer now selects this verified
checkpoint. `run_installed_apps.sh` accepts an explicit directory and uses frozen
artifacts when present, with eight CPUs and 4 GiB RAM by default. The older
`apps-ready-20261014` recipe is labelled historical. Writable file MAP_SHARED,
mapped truncate, the two renewed full bootstrap paths and full workload acceptance
remain unfinished.


### Full-plugin GIMP run: loader ENOMEM under investigation

The isolated `gimp-full-20261016` candidate uses production kernel
`33a6162e31bc48cabcf52c90018ebde8b51f2a1c16c958a2e8bc338c55d20473`, with the stale
global reduced-plugin override removed and explicit persistent HOME/XDG paths.
GIMP reaches its startup window, but many queried plugin processes fail loading
libgimp/libgimpui/libc and exit 127. A separately traced sparkle process reproduces
`mmap(0,571960,1,0x802,4,0) = -12`. This is a failure, not GIMP acceptance.

At a later all-vCPU-paused snapshot, the old vnode-keyed mapping-pin registry held
596/1024 entries, 2191 pins, zero resizing entries and 286 distinct inode identities.
This snapshot does not prove occupancy at the failing instruction, so table
exhaustion remains unconfirmed. Guest physical memory had roughly 2.5 GiB free;
heap integrity passed, with roughly 20 MiB of heap, 21005 blocks, 4.6 MB free and
a 3.6 MB largest free block. Opt-in mmap failure-stage diagnostics are the next
step, rather than increasing a limit without attribution.

The startup capture has 13087 CPU samples, 26931 context-switch records and no
dropped records. File-gate collection and allocator paths stand out in GIMP;
IRQ-disabled sampling bias still limits allocator attribution. The guest also
reported one saturated compositor-interval maximum (`4294967295` ms); investigate
clock/interval arithmetic before treating that sample as a real stall duration.
The VM stopped with checked filesystem/device flush and all three offline checks
passed. Evidence is in that candidate's `outcome.json`, `mapping-pin-capacity.json`,
`fsck.json` and preserved serial log.


### Mapping-pin exhaustion confirmed and corrected in source

The errors-only diagnostic run `gimp-mmap-trace-20261016` identified
`mmap-file-error ... stage=mapping-pin error=-12` for libgimp/libgimpui. In that
source version, the only such VfsFileMapPin failure was exhausting its fixed
1024 vnode slots. A clean standalone sparkle plugin loaded those libraries before
the full GIMP run filled the registry. This supersedes the earlier inconclusive
post-failure occupancy snapshot; see `diagnosis.json` and `failure-stages.log`.

The replacement `MappingPinRegistry` dynamically indexes vnode bindings and
aggregates identities by device/inode. Clone/unpin allocate nothing, allocation
and free occur outside the registry IRQ lock, and existing namespace/resize
exclusion and conservative FAT directory checks remain. The new hosted actual-
module test covers 4096 bindings/identities, concurrency, allocation failure and
external mutation exclusion. All eighteen hosted tests pass.

File-gate snapshots also now hash distinct files and heapsort unique pins outside
AS mutation, preserving the inline eight-file path, lifetime pins, ordered file
gates and generation revalidation. Tests cover repeated aliases, different backing
descriptors for the same file and dynamic-buffer reuse. Production and standalone
test builds pass after adding the registry dependency to standalone test_vfs.
Actual full-plugin GIMP acceptance and new startup timing remain in progress in
`gimp-mapping-fixed-20261016`; passing source/host gates alone does not establish
that GIMP or both full bootstrap paths now work.


### Heap measurements during full-package GIMP startup

`heap-timing-20261016` freezes kernel
`7cef04045077b1b733a3cff9288e6a5fdd4fba267c75d909d751c31855d7eb36`
(ABI 14), using the same source images as the preceding mapping-fix run. The
8-vCPU, 4-GiB VM used virgl with audio disabled. Heap timing is opt-in and uses
raw serialized TSC cycles; these totals have not been converted into wall time.
No host build or other VM ran concurrently, and no screenshot/debug pause occurred
inside either measured interval.

| Measurement | Idle, 10 seconds | GIMP startup, first 30 seconds |
| --- | ---: | ---: |
| Allocations | 597 | 154019 |
| Frees | 597 | 141200 |
| Allocation search headers | 40931 | 1434673087 |
| Maximum search headers | 317 | 23586 |
| IRQ-disabled lock wait cycles | 102137 | 2721946670 |
| Lock hold cycles | 10018545 | 33767004072 |
| Maximum lock hold cycles | 111895 | 3214671 |
| Expansion attempts | 0 | 2 |
| Expansion cycles | 0 | 827542 |

The loaded run examined about 9315 headers per allocation; hold cycles were about
12.4 times wait cycles. The two expansion attempts contributed little to hold
cycles. This directly supports replacing the allocator's scan of every physical
block with a free-block index. Totals cover the whole VM, including debug
transport, and the enabled instrumentation adds overhead. They are not a
per-process CPU percentage or proof of the complete startup critical path.

Default-off, enable/reset, disable with unchanged subsequent counters and heap
integrity passed on the live VM. The actual standalone QEMU tests for heap, PMM,
VMM and VFS pass after repairing the standalone build's accidental use of hosted
CR3/direct-map/IRQ stubs. Heap tests exercise IF-disabled preservation, timing
counters, real expansion, oversize rejection and failed-realloc data retention.
All eighteen hosted suites pass. Heap arithmetic now rejects requests that cannot
fit boundary tags, and a failed expansion releases its installed leaf frames.

GIMP was still querying plugins when this bounded measurement ended. The VM was
stopped with checked filesystem/device flush; this is performance evidence,
not completed image-edit/export acceptance. Raw reports are in
`build/workloads/heap-timing-20261016/{idle-timing,gimp-30s-timing,post-measurement}.json`.


### Free-block index comparison and remaining GIMP failure

`heap-index-20261016` freezes kernel
`2582aecebe22c0b071cb37ec1b8b6e2d90ca14c0577e0551fc54816c6c2c5f58`.
Both candidates cloned `gimp-mapping-fixed-20261016` and used the same launch
command/configuration and measurement windows. The new 32-bin free-block index
uses existing header padding; headers and footers remain 64 bytes each. Allocation
examines only free blocks, with ordered size classes and a nonempty bitmap;
boundary tags still drive coalescing. Integrity checks verify both the physical
chain and exact free-index membership.

The indexed 30-second run made 620791 allocations and examined 845904 headers
(maximum 54), about 1.36 per allocation versus 9315 in the baseline. Total lock
hold cycles fell from 33767004072 to 387567875 (87.1-fold), despite four times as
many allocations. This is not an 87-fold app startup improvement: the fixed windows
contain different amounts of work. Sixteen expansion attempts cost 152587891
cycles, including one large outlier; raw TSC can include host descheduling.
The idle run made 603 allocations, each examining one header. Heap integrity
passed during plugin discovery. Actual standalone heap/PMM/VMM/VFS tests and all
18 hosted suites pass; allocator tests include 4096 live blocks, one-header reuse,
class boundaries, coalescing, poison reseeding and 12000 mixed byte-oracle operations.

Full GIMP acceptance still **fails**. Faster plugin discovery exposed retained
blocked helper threads: 136 processes consumed about 3.36 GiB at one snapshot.
Later forks failed and GIMP faulted inside a tracked anonymous mapping. Preserve
this as a separate OOM/lifecycle investigation, not proof the fault itself is
fully explained. Many helper threads remained blocked after exit_group timeouts.
Source review found untimed futex waits can retain a protected-operation latch
from user-buffer validation, defer terminal publication, and never wake to reach
the syscall cleanup boundary. A focused cancellation repair and deterministic
guest regression are being validated. The VM stopped with checked flush; logs,
profile, process snapshot and numerical comparison are retained in the candidate.


### Untimed futex cancellation regression

The focused `futexexitcheck` fails on preserved kernel `2582aece` in round zero:
exit_group leaves four off-CPU waiters pending and the parent cannot reap the
group. Repaired kernel
`737efba78760ca34c7f701542cb0a3d4566ec3c10f731aa17a31eacb1a5e1f39`
passes all 32 rounds in Brook, with four untimed private futex waiters and 32 MiB
of touched anonymous memory per group. MemFree changes from 3932484 KiB after
warm-up to 3932412 KiB at the end (72 KiB difference); process count returns to
10. This bounded recovery check does not establish every individual PMM reference.
The existing mixed futex/pipe/sleep exitgroupcheck also passes 32 rounds.

Explicit `SchedulerBlockInterruptible` registration and the pending-cancellation
check share the scheduler ready lock. Terminal publication wakes only registered
interruptible waits (or stopped protected owners), letting the syscall remove
its own waiter/registrations and unwind before the terminal boundary. Ordinary
mutex/filesystem ownership waits retain their prior blocking behavior. Futex,
poll/select and epoll use this mode; common poll interruption cleanup now also
removes DevTty registration. Successful source builds and 18 hosted suites are
recorded in `interruptible-wait-validation.log`. Full GIMP retry remains separate.


### GIMP basic workflow and reboot persistence pass

On `interruptible-wait-20261016` with kernel `737efba78760ca34...`, the full
package reached its main window. Via VM mouse/keyboard input, the test created a
1920×1080 image, painted a stroke, saved `/data/home/brook-gimp-acceptance.xcf`,
and exported `/data/home/brook-gimp-acceptance.png` through the PNG plugin dialog.
Normal quit returned process count to the ten-process desktop baseline; heap
integrity passed. Relaunching the XCF preserved the stroke, and importing the
exported PNG through GIMP also preserved it. Both runs quit normally.

All three offline filesystem checks passed. Read-only extraction independently
verified the PNG decodes at 1920×1080, contains the nonwhite stroke, and is 50732
bytes (SHA256 `f6d8fbbc5688d6273475c1436c939bb0ccbf5ef939a2c2b66e6ac0964a6b99bb`).
The XCF is 224951 bytes with a valid XCF header; its decoding was verified in
GIMP itself. A complete VM restart of the same disks reopened the XCF with the
stroke intact and preserved the disabled welcome-screen setting. Both VM sessions
stopped with checked filesystem/device flush. This establishes the basic workflow,
not complete GIMP compatibility: several optional plugins still fault during
initial discovery on writes to tracked file mappings. Preserve those diagnostics
for the next mapping investigation.

Evidence includes `gimp-workflow.json`, `saved-files-verification.json`,
`gimp-png-reopened-1920x1080.png`, both serial logs and saved documents in the
ignored candidate directory. The screenshot is a native 1080p capture; no showcase
production has begun. Final source review found no blocking issue in the
interruptible registration/lifetime/unwind paths; zombie address-space retention
until parent reaping remains a separate improvement opportunity.

### Preserve synchronous fault addresses during diagnostics (2026-10-16)

The actual mapping-race probe now installs a SIGSEGV `SA_SIGINFO` handler and
requires the denied access's exact address before exiting. This exposed a
previously missed failure: the faulting access targeted `0x1016a000`, but its
signal frame reported `0x40d9b4`. A recoverable nested kernel CoW fault while
resolving or diagnosing the original exception can overwrite CR2 even with IRQs
disabled. Reading CR2 again after the register/stack dump therefore loses the
original address. The earlier unhandled-signal check only verified SIGSEGV.

Both exception handlers now snapshot CR2 before helper calls. The full handler
uses that saved address for resolution, diagnostics, panic metadata, `si_addr`,
ucontext and crash metadata, and passes it to the fallback reporter. This
changes no IRQ policy. The separate user-page-fault interruptibility experiment
remains unapplied; its slower rebuild measurements are in [bootstrap.md](bootstrap.md).

The combined test candidate passed eight complete race-probe invocations (176 cases)
across data and Nix disks, plus ordinary-I/O coherence, filesystem exit,
32-round futex and mixed-wait exit checks, fatal SIGSEGV reaping and heap
integrity. Checked device flush and all three offline filesystem checks passed.
This candidate also includes the separately evaluated growing file-page index;
its benchmark results must not be inferred from these correctness checks.
Evidence: `build/workloads/file-index-races-20261016/audit.json`; the original
failing `pre-cr2-fix-serial.log` is retained alongside it.

The framebuffer alias probe now queries geometry before mapping `/dev/fb0`,
which creates its own composited window buffer. Previously it wrote a canary
into live firmware scanout memory and could race compositor redraws. The
corrected probe passed 512 writable aliases, descriptor close and progressive
unmapping while checking surviving aliases.

### Debug transport reconnect and log framing (2026-10-16)

Compiler acceptance exposed intermittent loss of the guest debug connection.
The debug log streamer copied 160-character ring rows into 161-byte buffers,
then appended CRLF past their end. It now frames each row into separate storage
and handles partial sends, including structured command replies.

Passive TCP children now transfer one reference from their creator to the
listener queue, then to the accepted caller. Queue topology has a dedicated
guard; disposal occurs after unlocking. Initial SYN-ACK transmission and logging
finish before publication, so the creator cannot become a blocking last closer
in receive context. Reset/expired children are retired during accept and new
SYN admission, preserving buffered requests when FIN precedes accept. Handshake
handling prioritizes RST over ACK, checks the final ACK, retransmits the original
SYN-ACK on duplicate SYN and bounds abandoned passive opens to 30 seconds.
The old capacity-recovery path that forcibly freed live CLOSE_WAIT sockets was
removed: peer FIN does not surrender an application's descriptor or unread data.
General receive tuple lookup versus socket-slot reuse remains separate work.

The actual TCP state machine passes 77 assertions, including new passive-open
cases; the previous implementation fails eight assertions in the same expanded
test. An isolated eight-core, 4-GiB VM passed 12 rounds with two queued reset
clients, six queued buffered-request/FIN cases, 16 structured reconnects and
40 complete 160-character kernel-ring rows. Tests synchronize on the guest's
captured ACK rather than assuming host-forwarded connect means guest acceptance.
The same final candidate passed 176 mapping-race cases, both disk coherence and
filesystem-exit checks, private framebuffer aliases, futex/mixed-wait exit,
fatal signal reaping and heap integrity. Checked shutdown and all three offline
filesystem checks passed. Evidence and private captures remain ignored in
`build/workloads/debug-tcp-final-races-20261016/audit.json`.

### Export preparation for the next stock workloads

`tools/workload-pkg` now has individual export expressions for Blender, GIMP,
OpenTTD, zsh, kitty, starship and fastfetch, using the existing SDK package pin.
For example, export OpenTTD into the private package cache with:

```sh
nix-shell --run 'python3 scripts/export_guest_package.py --expression tools/workload-pkg/openttd.nix --name brook-openttd --version 15.3'
```

Nix evaluation resolves these selectors to Blender 5.1.1, GIMP 3.0.8, OpenTTD
15.3, zsh 5.9, kitty 0.46.2, starship 1.25.1 and fastfetch 2.62.1. This verifies
package selection, not guest compatibility or installation of those new apps.
The existing GIMP acceptance above remains a separate result. No default shell
or terminal has changed; interactive acceptance is required first.

### Files typography and OpenTTD, 16 October

The Files package now bundles Noto Sans Mono and rasterizes its ASCII glyphs
once with FreeType. Drawing blends the cached coverage; it does not open fonts
or rasterize glyphs on each frame. The original bitmap font remains the fallback.
The native Files, Calculator and media-player launch scripts now resolve their
executables through `/nix/profile/bin`, so guest package upgrades take effect.

An isolated eight-core, 4-GiB VM installed Files 0.2 and stock OpenTTD 15.3 through
the guest installer in 79.2 host seconds. Files reported the Noto font ready and
passed pointer directory navigation. OpenTTD rendered a new 256×256 world with
the software SDL backend, opened the town directory, maximized, saved a game
and loaded it again. Sound and music were explicitly disabled for this run.
Both applications closed normally; checked shutdown and FAT/ext2 checks passed.
Evidence is in ignored `build/workloads/desktop-font-workloads-20261016`.

OpenTTD initially lacked text events because SDL 3.4.2 requires
`zwp_text_input_v3` to activate text input. Brook now exposes version 1 of that
protocol with per-resource focus, buffered activation and lifetime cleanup.
Physical keys remain on `wl_keyboard` for client XKB/compose handling; an input
method service, preedit and on-screen keyboard are not implemented.

The compositor package runs a host test against real Wayland resources and wire
events. A second VM passed console typing and command execution, loading the
previous saved world, and saving `Brook-input-acceptance.sav` by typed command.
Mousepad saved exactly the 46 injected bytes, including mixed case, digits and
punctuation, with no duplicated characters. Focus returned to OpenTTD after
Mousepad closed. Both apps exited, heap integrity passed, checked shutdown and
all disk checks passed. Evidence is in ignored
`build/workloads/text-input-20261016/revised-acceptance.json`.

Remaining issues: the OpenTTD save dialog reports zero free bytes because
Brook's `statfs` is a zero-filled stub. GTK Save As absolute-path completion can
stop accepting input when its popup appears; a control run with the original
compositor reproduced this, while saving a simple filename worked. Scheduler
watchdog messages during package decompression also need separate investigation;
both installations completed without intervention.

### Stock shell tools and compatibility fixes, 16 October

Stock zsh 5.9, fastfetch 2.62.1 and starship 1.25.1 installed through the guest
package installer. The initial run exposed two kernel defects:

- `sigsuspend` restored the caller's blocked-signal mask before signal delivery.
  Zsh repeatedly returned from the wait without running its SIGCHLD handler,
  leaving exited pipeline children unreaped. It now uses the same temporary-mask
  return path as ppoll/pselect/epoll_pwait, preserving the original mask in the
  handler's ucontext. The old kernel fails both queued-signal and child-exit
  delivery checks; the fixed kernel passes all 32 wait-mask cases.
- Ext2 stored a 60-byte symlink target in the 60-byte inline area, leaving no
  terminator space. Zsh's relative profile link hits this exact boundary, and
  offline fsck rejected it. Targets of 60 bytes now use a data block. Six target
  lengths around the boundary pass exact size/readlink checks, survive reboot,
  and pass offline fsck; the old-kernel control produces an invalid 60-byte link.

Fastfetch also revealed inflated RAM reporting: sysinfo and `/proc/meminfo`
used the highest tracked physical page, including address holes. They now use
the existing RAM-page count. Fastfetch JSON reports 4,237,357,056 total bytes,
matching the VM bridge, and the new `/etc/os-release` identifies Brook.

`scripts/workloads/shell.sh` checks zsh arrays, globbing, file read/write and a
multi-process pipeline; fastfetch text/JSON output; starship prompt generation
and generated zsh integration syntax. Stage it as `/boot/SHELLACC.SH` and
`data/fastfetch/brook.ascii` as `/boot/BROOK.ASC`. It uses a data-backed temporary
directory because open-file unlink under the boot filesystem's `/tmp` still
fails. Require `SHELL_WORKLOAD_EXIT_0` with no manual intervention.

An eight-core, 4-GiB VM passed the script before and after reboot. Interactive
zsh accepted commands in the native terminal, sourced starship's generated
integration, displayed its prompt, and ran fastfetch with the Brook ASCII mark.
All nested shells exited normally, heap integrity passed, both shutdowns used
checked flushes, and FAT/data/nix checks passed after each run. Evidence remains
in ignored `build/workloads/shell-fixed-20261016/acceptance.json`.

The same frozen kernel passed the short eight-worker RAM-backed I/O gate in
21.77 host seconds, including random pread/mmap, inode-lifetime, mixed append,
the expanded wait-mask/child-signal/select probes, reboot verification and exact
free-block/inode recovery. Evidence is in ignored
`build/io-bench/shell-fixed-d78482b6-ram/result.json`. These are RAM-backed
stability/iteration timings, not SSD throughput measurements.

Shell defaults have not changed. The native terminal leaves zsh's prompt-end
marker visible; Kitty, job control and broader interactive behavior still need
acceptance. The original failing clones and profile are retained separately,
and their success markers after manual interruption do not count as acceptance.

### Firefox and Codium rerun on 4e53a2a, 16 October

Both apps were rerun with software graphics, eight vCPUs and 4 GiB RAM on the
accepted kernel `d78482b62b04d08ae517d861515534754893323f6fa64bf2d0070a20239c613d`.
Private image clones and fresh application profiles preserved the default app
checkpoint. Firefox 150.0.1 installed through the guest package installer.

Firefox drew tabs and the address bar, but the visible page remained blank and
typed address-bar text did not appear, including after clicking the field. Its
Wayland title nevertheless became `Brook JS ready — Mozilla Firefox`: that
title is set only by JavaScript in the local test page, establishing execution
of that script despite the unusable display. A five-second capture saved 3,922
samples and 41,472 context switches; the busiest sampled app thread was repeatedly
calling clock_gettime. This does not establish the rendering failure's cause or
attribute it to writable shared-file mappings.

SIGTERM did not complete Firefox teardown; SIGKILL returned the VM to its ten
kernel/Wayland processes. Teardown emitted `BRO176-MAPLEAK` and
`BRO176-MAPUNDERFLOW` for physical page `0x14d3a000`. The run therefore fails
kernel memory acceptance even though heap metadata and all offline disk checks
passed. Preserve this as a separate ownership failure, not merely blank content.

Codium 1.112.01907 connected to Wayland but showed no editor and hit user-mode
INT3 at runtime RIP `0x6efdcbd`. A second boot tested Codium alone with a fresh
profile, avoiding the preceding Firefox memory violation. Memory-error tracing
captured a 262,144-byte writable MAP_SHARED request returning `EOPNOTSUPP`, then
the same breakpoint. The exact ELF has load bias `0x200000`; the INT3 instruction
is at file VA `0x6cfdcbc`, with saved RIP immediately after it. Missing inotify,
netlink and D-Bus/udev support also remain visible, without establishing that
each is fatal. The clean Codium run returned to ten processes after SIGKILL,
passed heap/page diagnostics and all offline disk checks, and stopped with a
checked flush. Neither app is accepted as usable. Evidence, traces, profile and
screenshots are in ignored `build/workloads/browser-rerun-20261016/result.json`.

### Unix descriptor ownership exposed by Firefox teardown (16 October)

A small `unixrightscheck.c` reproduces the Firefox teardown memory warning
without the browser: one sender passes 512 references to a mapped memfd while
four receiver threads receive and close them. Linux receives exactly 512.
The old Brook kernel installed 577 descriptors, then freed the still-mapped
memfd and reported a mapping-count underflow. The prior-profile Firefox control
also reproduced a teardown panic; the first erroneous free came from descriptor
close, rather than memfd truncation.

The direction's pipe lock now protects byte publication/consumption and the
associated descriptor queue together. Rights are published with their first
actual byte, transferred once, and kept behind their stream position. A failed
write no longer rolls back references that a receiver may already have consumed.
Plain reads and truncated control buffers close discarded rights; received
CLOEXEC follows MSG_CMSG_CLOEXEC. `sendmsg` stops after a partial iovec write.
The existing ancillary queue remains bounded to 63 descriptors per group;
unsupported Unix `recvmsg` flags return EOPNOTSUPP.

Kernel `d86f2927f0e0c192253684e8998ef1f9f4da2df2a0914a2dca458eef4ac037e4`
passes the one-receiver test and repeated four-receiver tests (512/512 each),
all 19 hosted tests, and two Firefox launch/TERM/KILL cleanup cycles. Each cycle
returned to the ten-process baseline with no page ownership warnings. Checked
shutdown and offline FAT/data/Nix checks all pass. Evidence is under
`build/workloads/firefox-rights-fixed-20261016/`, session
`ba9e465c97074bdc83dfb9e4815f462d`. The failed diagnostic disks are preserved
separately; their unsynced panic left data fsck status 4 and they were not reused.

Firefox still displayed a blank page and stale toolbar after the ownership fix.
Wayland tracing confirms keyboard events reach the client. Rendering and input
acceptance remain separate from this memory-lifetime correction.

### Firefox rendering, input and verified HTTPS (16 October)

Firefox 150.0.1 now renders the local JavaScript test and real websites on the
software-graphics, eight-vCPU/4-GiB candidate. The compositor previously stranded
frame requests on commits without a new buffer. Committed callbacks now run from
a shared 16-ms frame clock, with resource-owned lists instead of a fixed eight
callback array. This is a pacing timer, not a presentation-feedback guarantee.

Firefox also binds wl_seat twice: GTK and Gecko use different bindings. Sending
input only to the newest binding left the visible toolbar unresponsive. Focus
and input now reach every seat binding belonging to the target client, while
window-management actions run once. Wheel-axis direction now translates the
kernel's positive-up convention to Wayland's positive-down convention. A real
libwayland wire test covers commit eligibility, repeated bufferless commits,
callback lifetime, multiple seat bindings, foreign-client isolation and axis sign.

NSS/p11-kit could not find its standard trust path despite the Nix CA bundle
being installed. The initrd now contains /etc/ssl/certs/ca-certificates.crt from
the pinned development shell's cacert package. A missing bundle fails before
image writes; bundle roundtrip and existing-image preservation checks pass.
No certificate-verification bypass is used.

The initial candidate browsed Google search, the Brook GitHub repository and
its docs folder, and Amazon UK search/product pages, including images and clicks.
No accounts, purchases or checkout were exercised. After a fresh boot with the
final package, the local test registered one click and exact typed text/JS output;
Google HTTPS search rendered without a manual certificate copy; wheel-down
scrolled down; and Mousepad saved exact text without doubled input. Firefox
quit normally with exit 0, leaving ten baseline processes. Heap integrity, page
ownership checks, checked shutdown and all three offline filesystem checks pass.

Evidence: ignored build/workloads/firefox-fresh-20261016/acceptance.json, session
21561c0aab7c4a58a4f25eeeb1c6cee8, kernel d86f2927f0e0c192253684e8998ef1f9f4da2df2a0914a2dca458eef4ac037e4,
Wayland package dv0li9831cwqrh9i5jaavjcmg56b49im-waylandd-brook-0.1-brook.
The default current-apps checkpoint has not been promoted.

A preceding longer Google/GitHub/Amazon session closed its window but retained
347 workers for minutes and required guest SIGKILL. Its profile recorded 1,721
CPU samples, 59,522 context switches and 119,835 dropped records; it is incomplete
and cannot establish elapsed-time percentages or a complete cause. The short
fresh-boot normal quit does not close that follow-up. Evidence remains separately
in build/workloads/firefox-input-20261016/acceptance.json. Maximized client-side
decoration geometry also needs polish; this pass does not establish hardware
rendering, video playback, long-session stability or general browser compatibility.

### Chromium startup and anonymous discard (16 October)

The pinned stock Chromium 148.0.7778.96 closure now reaches its normal Wayland
browser window on the eight-vCPU, 4-GiB software-graphics candidate. The launch
uses `--no-sandbox --disable-gpu --ozone-platform=wayland`, a private data-backed
profile/TMPDIR and native `/dev/shm`. It does not use `--no-zygote`, single-process
mode or `--disable-dev-shm-usage`. The default current-apps checkpoint is unchanged.

The compatibility work fixes several independent startup failures:

- `readlink` distinguishes missing paths (ENOENT) from existing nonlinks
  (EINVAL), including filesystem roots, and resolves relative syscall paths.
- `mkdir` passes the requested creation mode through the filesystem interface;
  ext2 publishes private 0700 directories with the correct umask and reports
  EEXIST. The filesystem module ABI is now 15. FAT and the experimental lwext4
  adapter do not acquire ext2's permission contract. Linux `creat` is dispatched
  through ordinary write-only create/truncate handling.
- The initrd provides `/dev/shm`. Named shared-memory open/unlink uses dynamic
  namespace entries with independent backing references: unlink/recreate does
  not invalidate old descriptors or mappings. Path stat/readdir and complete
  tmpfs permissions remain outside this subset; mapped truncation is unsupported.
- `execve("/proc/self/exe",...)` resolves the process's recorded executable
  pathname. This is not an inode-pinned magic link surviving rename/unlink.
- Invalid eventfd2 flags return EINVAL before allocating a descriptor. Chromium
  intentionally probes invalid flags; the old false success caused a Mojo
  fatal check. FD_CLOEXEC/status flags are published with the descriptor, and
  counter reads use compare/exchange so concurrent writes cannot be lost.
- Automatic SO_PASSCRED credentials are attached to queued bytes using the
  actual sender's PID/real UID/GID. An inherited socket's connection-time peer
  PID is insufficient for Chromium's child-PID oracle. Credential spans and
  descriptor ownership share the pipe lock. Explicit user-supplied credentials
  remain EOPNOTSUPP; full SEQPACKET boundaries remain follow-up work.

Chromium then reproducibly faulted in the pinned libgobject's `g_closure_unref`
with a `0xbadbad00badbad20` notifier pointer. Brook's madvise was a success-only
stub: a guest Python negative control wrote `POISON!!`, called MADV_DONTNEED,
and read the same bytes. Private anonymous discard now replaces populated frames
with zeroed frames under an address-space transaction, preserving permissions,
hidden PROT_NONE leaves and fork peers. It preallocates before mutation and
retains detached frame references until full TLB acknowledgement. Sparse empty
reservations are walked by allocated page tables. It currently replaces resident
frames eagerly; it does not promise RSS reclamation. File/shared/device discard
is unsupported. Other content-changing/fork-inheritance advice returns an error.
With this correction, the GObject crash no longer occurs in the recorded run.
Writable regular-file MAP_SHARED remains disabled; this work does not expose
the pending shared-write/writeback foundation.

One failed Chromium startup also exposed a scheduler ownership assertion. Exit
cleanup could block/migrate but retained a CPU index captured before cleanup.
Final exit dispatch now obtains that index under the ready lock; yield selects
CPU/current process under the same lock, including its idle fallback. The public
current-process helper uses the existing single GS-relative lookup. The failed
VM and raw memory captures are preserved separately; forced-stop fsck returns 4
on its data/Nix disks, which were never reused. The following scheduler candidate
passes 16 filesystem/exit rounds and 32 mixed thread-group exit rounds, and both
failed application launches clean up to ten processes without a kernel panic.

All 19 hosted suites were rebuilt and pass, including an actual-VMM discard
test covering allocation failure before publication, fork isolation, hidden
leaves, bounded snapshots and delayed TLB acknowledgement. In Brook, that
candidate passes `discardcheck` (60 checks, eight readers, 32 rounds),
`passcredcheck` (887 checks), named SHM with 96 live names, eventfd concurrency,
six executable-identity cases, permission checks on data/Nix, thirteen readlink
cases, file coherence and 512/512 concurrent SCM_RIGHTS transfers. The new
discard/credentials probes also pass on Linux.

The local page visibly executes JavaScript, counts a click and echoes exactly
`Chromium inside Brook`. Google HTTPS search renders and its consent dialog
responds to input. Wikipedia renders text and images; clicking its History
contents link updates the fragment and scroll position. The Brook GitHub page
loads but some stylesheets/images are missing or delayed, so full GitHub visual
acceptance remains open. No TLS bypass, account sign-in or purchase was used.
Normal Ctrl+Shift+W quit returns `CHROMIUM_APP_EXIT_0`, ten desktop processes,
about 3.97 GB guest free memory and a healthy heap. Recent compositor intervals
are around 16 ms; those are not browser rendering benchmarks. Maximizing still
leaves the client displaced beyond the screen and needs compositor geometry work.

Evidence is under ignored `build/workloads/chromium-discard-20261016/`, session
`4f61644ed2ef403dac8e36acce28c59c`, kernel
`67ed71997f0a607a8705e77a667bb03ae14bc17b14a5598f53b4e9d3d0f17c7c`.
Native screenshots include the local interaction result and Wikipedia's History
section. Earlier negative controls remain in the chromium-* candidate folders.


### YouTube playback and descriptor capacity (16 October)

Firefox renders actual YouTube video at 854×480 on the software-rendered guest.
Distinct decoded movie frames and media times of 42.72, 83.98 and 121.24 seconds
establish playback beyond the player thumbnail. The unmuted media element reports
an audio track, but Firefox logs `OpenCubeb() failed to init cubeb` and
`OnMediaSinkAudioError`; audio output is not accepted. Silent-backend PCM capture
is independently verified with a guest test tone: 136,193 stereo frames at
44,100 Hz, about 3.09 seconds, with nonzero signal. This capture does not use the
host microphone or speakers.

The earlier 4-GiB candidate subsequently exhausted memory while loading another
Chromium helper after opening YouTube/DevTools, then panicked in an unchecked
signal-frame write. Its preceding normal Chromium/Firefox quits remain useful
workflow evidence, but the whole candidate fails stability acceptance. Its disks
are preserved and are not reused. Further video testing uses clean cloned disks,
6 GiB of guest memory and eight vCPUs, with at least 8 GiB host memory headroom.

Chromium renders YouTube's page and player thumbnail. The first 6-GiB attempt
aborted when its 256-descriptor table filled and a shared-memory allocation
returned EMFILE. Descriptor and epoll capacities are now 1,024. The larger table
exposed an existing kernel-task exit bug: tasks without an FD table scanned a
null pointer. Exit/close-on-exec cleanup now skips absent tables, and kernel-AS
faults no longer enter user-mapping transactions. After this correction, the
guest boots and passes `fdcapacitycheck`: 512 eventfds, 512 poll/epoll watches,
unique readiness/drain checks and F_DUPFD_CLOEXEC at descriptor 900. The same
probe passes on Linux; all 19 hosted tests pass, including kernel-AS page-access
rejection. Chromium YouTube playback remains unverified after the retry.

Evidence: ignored `build/workloads/chromium-youtube-clean-20261016/` (thumbnail,
EMFILE and positive audio control), `chromium-youtube-fdcapacity-20261016/`
(preserved boot panic, no guest capacity test), and
`chromium-youtube-fdexit-20261016/` (corrected boot and capacity probe).


The corrected candidate closes normally (`CHROMIUM_APP_EXIT_0`), returns to ten
baseline processes, passes heap integrity, completes checked filesystem/device
flushes and passes all offline FAT/ext2 checks. A direct HTTP MP4 control decodes
320×180 H.264 frames and plays AAC audio through the existing ALSA-to-`/dev/dsp`
configuration. Media times advance through the five-second clip and loop; decoded
frame counts rise from 227 to 397. A three-second silent-backend capture contains
136,151 stereo PCM frames, RMS 0.038. This is browser-produced audio, independently
of the earlier kernel test tone. ALSA fallback warnings alone do not establish
failure. Firefox's cubeb failure remains a separate open finding.

YouTube's AV1/Opus attempt still stays at media time zero with no decoded
resolution. Resource Timing records HTTP 200 and 1,073,775 transferred bytes for
`/videoplayback`; this does not establish that all subsequent media requests work.
A local fragmented H.264/AAC MediaSource fixture also stalls. The identical file
and page on host Linux reach readyState 4 and 295 decoded frames in ten real
seconds. A fresh isolated guest run reaches readyState 4 and four decoded frames,
then remains at media time zero. A later navigation may fail to dispatch
`sourceopen`. These distinguish the streaming pipeline failure from the accepted
direct-file decode/audio path; they do not yet identify its syscall root cause.
The initial YouTube stall profile has 781 idle samples out of 802, rather than
sustained decoding CPU pressure. The focused rerun is preserved under ignored
`build/workloads/chromium-mse-20261016/`.


### Chromium YouTube video and audio: diagnostic launch (16 October)

The focused retry verifies actual YouTube playback with
`--autoplay-policy=no-user-gesture-required` added to the isolated launch. The
same six-GiB/eight-vCPU software-rendered VM reaches 101.347 seconds, 854×480,
readyState 4, unpaused/unmuted at volume 1, with 3,044 decoded frames. Earlier
samples show 0.52 and 30.36 seconds; these are actual movie frames, not the
thumbnail. A fresh browser restart with the saved cookie rejection also plays.
The native 1920×1080 capture shows the movie in a roughly 1500×900 browser window;
`Browser.setWindowBounds` avoids the existing maximize-placement issue.

YouTube audio also works in this run. A fresh three-second silent-backend capture
contains 135,844 stereo frames at 44,100 Hz, duration 3.080 seconds, RMS 0.0552
and no silent plot bins. Speakers remain unused. This establishes output, not
full audio quality or absence of intermittent stutter. The browser closes with
exit 0, ten baseline processes and a healthy heap; checked shutdown and all
three offline filesystem checks pass.

The autoplay flag is a diagnostic workaround, not a general playback fix. Under
the ordinary policy, the minimal MSE fixture can decode four frames and then
stall at time zero; its saved media log stops in `kSuspending`. Under the diagnostic
policy, video-only MSE loops past 1,400 frames and the combined H.264/AAC fixture
plays with nonzero captured audio. A short pause/resume in that fixture passes;
a longer YouTube pause/resume still stalls. Do not classify all MSE, audio or
YouTube interactions as accepted from this run.

Source inspection found `sys_shutdown` still returns success without performing
socket shutdown. This is a candidate cause of media-thread cancellation hangs;
no causal reproduction or shutdown implementation is claimed here. Implement
and test real Unix socket half/full shutdown and wakeup semantics before removing
the workaround or declaring suspension/resume reliable.

The compatibility checkpoint is commit `56175f8`. Playback evidence and exact
launch scripts are under ignored `build/workloads/chromium-mse-20261016/`, session
`3a253330c455439abf69342010a3a800`, with structured `acceptance.json`, guest media
logs, CDP state snapshots and the local fragmented-MP4 fixtures. The clean native
screenshot is `1cbd212eb87b407cae9d668a44a877e5.png`; the final YouTube PCM capture
is `ca59111f596b4f5c97159cc44b10a51c.wav` in that session's artifacts. No media,
private profiles, VM disks or copyrighted Quake data are added to Git.


### Unix shutdown and normal-policy Chromium playback (16 October)

Connected AF_UNIX `shutdown()` now changes stream direction state instead of
returning success without doing anything. Shutdown is separate from pipe
ownership: descriptors, blocked operations and ancillary queues keep their
references until actual close. SHUT_WR rejects further sends, allows the peer to
drain queued bytes/SCM_RIGHTS and then read EOF, and preserves reverse traffic.
SHUT_RD wakes local readers and rejects peer sends, retaining already queued
bytes for reads as Linux does. Repeated shutdown through duplicate descriptors
does not decrement ownership counts. Poll and epoll share the same EOF, RDHUP,
HUP and writable-error readiness rules; half-close does not falsely signal full
hangup. SIGPIPE and MSG_NOSIGNAL follow write/send behavior. The send path's
MSG_DONTWAIT no longer temporarily changes a shared socket's nonblocking state.
Internet sockets and unbound/listening Unix sockets remain unsupported and
return EOPNOTSUPP, rather than reporting a fictional successful shutdown.

`shutdowncheck` passes 722 checks on Linux and Brook. Eight rounds exercise four
blocked readers plus poll/epoll waiters, and four blocked writers with a full
queue; the close race shuts down via an alias after closing the descriptor still
pinned by writers. Buffered rights survive shutdown and are delivered once.
Explicit SIGPIPE delivery and MSG_NOSIGNAL suppression are checked. The existing
browser prerequisite suite also passes: descriptor capacity, anonymous discard,
automatic credentials, named SHM, eventfd, exec identity, permissions, readlink,
file coherence and concurrent SCM_RIGHTS. All 19 hosted suites pass.

With the same software-rendered six-GiB/eight-vCPU setup, Chromium now plays the
small H.264/AAC MediaSource fixture under normal autoplay rules. A user gesture
starts playback after autoplay is denied; frame counts advance instead of
freezing at four frames. The media log records completed `kSuspended` and
`kResuming` transitions. A 35-second pause resumes to more than 2,700 decoded
frames, with nonzero PCM output. The diagnostic autoplay flag is no longer
needed for this workflow.

YouTube also passes a normal-policy launch, optional-cookie rejection, explicit
play, a 35-second pause/resume, and seeking to 120 seconds. Recorded playback
advances to 147.822 seconds at 854×480, readyState 4, unpaused/unmuted at volume 1,
with 3,365 decoded frames. A three-second output capture contains 132,661 stereo
frames at 44,100 Hz, RMS 0.0693, with no silent plot bins. This establishes output,
not complete stutter-free audio quality. No host speaker playback is required.
The browser exits normally with status 0, returns to ten baseline processes and
passes heap integrity. Checked shutdown and offline FAT/ext2 checks are clean.

Evidence is under ignored `build/workloads/chromium-shutdown-20261016/`, playback
session `cede96356ecc43d8908bc5da341dc848`. It includes structured acceptance,
state snapshots, completed suspension events, PCM and a native 1920×1080
screenshot `cf5fe541b3c5468383e225117727972f.png`. The final output capture is
`5fa7117aea4444459ebbf11050534aca.wav`. Firefox audio has not been rerun; its cubeb
initialization failure remains the next audio investigation.

### Firefox audio isolation (16 October)

The isolated eight-vCPU/six-GiB candidate in
`build/workloads/firefox-audio-20261016/` used the same kernel as the accepted
Chromium shutdown run. Firefox 150.0.1 played a local H.264/AAC fixture, but the
default audio IPC path failed cubeb initialization. Setting the cubeb sandbox
preference false alone did not disable IPC: Firefox forced it back on while
the content sandbox was enabled.

A separate diagnostic profile selected ALSA and disabled content sandbox and
audio IPC. Its stream initialized and QEMU captured nonzero 44.1-kHz stereo PCM
without speaker output. This is **not working audio acceptance**: the three-second
capture contained extensive silence and Firefox repeatedly reported lost frames.
Investigate PCM position/delay reporting in the file-over-null ALSA route,
separately from the default IPC failure. No Firefox launcher defaults changed.

The script generated each complete preferences file in one write after shell
append unexpectedly overwrote the start of an ext2 file. That corruption is
preserved as a separate O_APPEND finding. Firefox reported normal exit, and the
kernel heap check passed, but child tasks remained after repeated launches.
Serial acceptance also found a virtio-blk timeout followed by a reported recovery.
The VM was stopped with checked guest synchronization; final offline filesystem
checks passed for FAT and both ext2 filesystems. Results and all diagnostic
evidence are retained with the candidate. Do not promote this candidate as a
clean acceptance image.


### Desktop launch, persistent home and idle audio (16–17 October)

The desktop now defaults to `/data/home`, with XDG configuration, cache and
data directories on the persistent ext2 volume. Boot creates missing directories
without replacing existing ownership. This remains a shared experimental desktop
identity, not complete account isolation: the shell is root and Firefox uses the
existing UID 1000 compatibility override. Account work requires the ownership and
permission audit recorded in the backlog.

A fresh volume exposed an ownership bug: `Ext2Chown` selected the first ext2
mount and ownership syscalls concealed missing-path errors. Ownership now routes
through VFS mount lookup; descriptor operations target the retained inode,
including unlinked files. Symlink and directory-relative variants are handled
separately. Unsupported filesystems and write failures return errors; this ext2
backend rejects IDs above 65535 rather than truncating them. The guest ownership
probe passed across `/data` and `/nix`, including set-ID clearing and errors.
Root and UID 1000 each wrote, fsynced and reopened 90,112 bytes in HOME; both
verified the same contents after a complete VM restart.

Installed browser entries now use deployed compatibility wrappers after checking
that the actual package exists. Chromium needs its explicit experimental
`--no-sandbox` software-Wayland setup and an ext2 temporary directory. Firefox
needs the compatibility UID, persistent profile and shared-memory setup. New
Firefox profiles receive first-run preferences once; saved preferences are not
overwritten. The native launcher now validates the executable after its supported
`--uid` prefix. A hosted regression brings the total to 20 passing suites; the
two desktop-discovery tests pass as well. The launcher uses the current B mark.

On eight vCPUs and 4 GiB with software rendering, GIMP 3.0.8 launched from its
generic installed desktop entry on the fresh-home reboot and quit normally. Its
configuration was written to the persistent home. The VM returned to ten tasks,
heap integrity passed, shutdown completed a checked device flush, and all three
offline filesystem checks passed. Session: `a7472453b2af4093bd56cc440f0b6b85`;
native 1920×1080 screenshot: `artifacts/a2235dec2c2a42b1814458a9d3f49d60.png`
under that session's bridge directory.

The HDA service now stops and clears cyclic DMA after its queue drains, even
when no writer makes another call. Bounded PCM tests captured nonzero audio,
inactive output during an open-descriptor idle interval, resumed audio, and
inactive output after a finite writer was killed and reaped. OpenTTD passed
both a main-menu exit and a generated-world confirmation exit with music: each
returned to ten tasks and inactive audio; heap and offline filesystem checks
passed. These bounded passes do not isolate the cause of the user's earlier
exit hang. Audio evidence session: `35fb828c92274a6f877edb6c7d288eb8`; OpenTTD
evidence session: `0162d1da84bf451f81fa5efef6495010`.

Firefox's fresh-profile menu launch rendered Example Domain, and an earlier
launch rendered interactive Google HTTPS results. **Normal quit still fails:**
the window closes but its tasks linger until the shutdown watchdog deliberately
crashes the process. Glean reports shutdown before initialization. Eventual
process cleanup is not a clean-exit pass. Audio IPC and direct-ALSA pacing also
remain open. The successful home/ownership checks do not accept Firefox shutdown.
Fresh-profile session: `3d845022e27540de8093243756fd608b`.

Build and diagnostic evidence is in
`build/workloads/desktop-regressions-final-20261016/` (ignored). The tested kernel
SHA-256 is `a9323368a99d45876327183c2dcf51d00b18baf9fa8ee5aa14645de9b01222c9`.
Kernel and all staged modules were matched after the VFS callback change.


### Codium shared-memory control (17 October)

VSCodium 1.112.01907 was rerun alone on the persistent-home kernel above, with
eight vCPUs, 4 GiB and software Wayland graphics. Putting `TMPDIR` on ext2
while retaining `--disable-dev-shm-usage` reproduced the exact breakpoint at
runtime RIP `0x6efdcbd`. Disassembly of the installed ELF ties its caller to a
failed `0x40000` allocation and the `FieldTrialAllocator` string. This identifies
the field-trial allocation path more specifically than the earlier nearby mmap
error alone. Chromium's [field-trial source](https://raw.githubusercontent.com/chromium/chromium/main/base/metrics/field_trial.cc)
also uses a 256 KiB shared-memory allocator and fatal allocation handling.

A second isolated boot removed only `--disable-dev-shm-usage` and used a fresh
profile. The breakpoint did not recur, and additional GPU/utility helpers
started. Brook implements `/dev/shm` files with its in-memory shared backend.
Chromium's [temporary-directory selection](https://raw.githubusercontent.com/chromium/chromium/main/base/files/file_util_posix.cc)
uses `/dev/shm` unless the disabling flag forces the temporary directory.
**Codium is still not accepted:** no editor window appeared before targeted
cleanup, roughly three minutes into the run. An 8,005-sample ten-second profile
contained 7,893 idle samples; the stall is not established as CPU-bound. Guest
used memory was about 1.66 GB. A bounded two-second syscall trace captured
clock, poll, futex and epoll waits; it does not establish the remaining cause.

Both runs returned to ten tasks after targeted SIGKILL, passed heap integrity,
stopped with checked filesystem/device flushes and passed all three offline
filesystem checks. No writable regular-file MAP_SHARED support was enabled.
Do not add `--disable-dev-shm-usage` back to the next Codium control command.
The test commands, syscall trace, profiles and result are preserved under
`build/workloads/codium-home-20261017/` and
`build/workloads/codium-shm-20261017/`; sessions are
`f8420feb131f456ea842e3316673d971` and `7ee8f37e962f4e12acdd431ec6623b1b`.
