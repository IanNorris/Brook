# Mount paths use the VFS path bound

Mount entries, rename preparation, mount snapshots and mountinfo rows now
consistently store paths up to the existing 512-byte VFS bound, including NUL.
The previous 64-byte mount-label bound rejected SteamRT's 83-byte working
directory export. Rename preparation checks the combined prefix/suffix length
before allocation or copying, including the exact 511-byte boundary.

ABI 46 changes the public mount snapshot layout. The kernel and all thirteen
modules are rebuilt and staged together. Procfs derives mountinfo's worst-case
escaped row allocation and mounts' row allocation/append guard from the actual
field sizes. This prevents the wider snapshot from overrunning an old row
budget. Existing mounts-format escaping and unconditional rw limitations are
unchanged; mountinfo remains the detailed interface.

Host tests cover long bind targets, rename to a longer ancestor, recursive
binds, namespace-copy detach isolation, full snapshot/mountinfo reporting,
allocation cleanup and exact rename-label boundaries. The portable probe
checks long O_PATH reports and complete names in both proc mount files before
and after rename, recursive bind and namespace copy. The O_PATH fstatfs fix is
included and its retained-descriptor tests run on the same candidate.

Final acceptance uses the ABI-46 kernel recorded in
`steamrt-runtime-checkpoint.md`. OpenAtInternal's user-path buffer now uses the
same 512-byte bound. Linux and Brook each pass 113 fixture plus five parent
checks. The initial Brook probe used unsupported `/proc/self/mounts`; that failed
operation is preserved, and the corrected Brook probe reads `/proc/mounts`.
