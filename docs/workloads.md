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
**Vim acceptance remains incomplete** until the timer path and exact long-input
case pass. Do not count dismissing the warning as compatibility.

Brook's current kernel terminal uses pipes and TTY ioctls. Full Linux PTY
support (`/dev/ptmx`, `/dev/pts`, allocation/unlock, controlling terminal and
hangup semantics) remains separate work. Helix and Emacs have not been tested
in this acceptance sequence. Codium, Blender and Firefox/Chromium also remain
outstanding.
