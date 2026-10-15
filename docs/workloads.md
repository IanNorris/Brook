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
