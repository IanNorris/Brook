# Retained Unix socket binds

The unique-entry bind subset now accepts Unix sockets as well as regular files,
including a socket over a regular placeholder as used by bubblewrap. Directory
mismatches still fail; hard-linked entries, devices and other special types
remain unsupported. Existing retention, stacking, namespace copy, recursive
bind, detach, mutation exclusion and source-reporting rules apply unchanged.
Socket source rename updates reporting in both retained and legacy rename APIs.

Unix listener lookup already uses the pinned backing device/inode. A bind alias
therefore reaches the original listener after its source name is renamed,
replaced or unlinked. Closing the listener makes new connections fail while
established streams own their independent endpoints.

Connecting to a socket does not modify its filesystem inode. Retained descriptor,
capability-path and pathname access checks allow its write-permission inquiry
through a read-only mount, then still apply backend permissions. Writes,
truncation, chmod, unlink and other filesystem mutations retain existing mount
restrictions. The legacy string-only access API retains its conservative behavior;
this is not a claim of uniform support across all legacy entry points.

The portable probe creates an Ext2/Ext4-backed listener and binds it over a file
inside a separate tmpfs instance. It checks O_PATH reports, data exchange,
source rename/replacement, stacking, read-only connections, recursive binds,
namespace-copy detach isolation, source unlink, listener close, surviving
established streams and a held metadata FD after detach. Dropped-capability
children must get EACCES with socket permissions removed through both RW and
RO aliases. Host tests additionally check reporting and reference cleanup.

Linux and Brook both passed 172 fixture checks plus five parent checks. Prior
tmpfs-mode and file-bind cases passed on the candidate. All 101 host tests,
ASAN/UBSan for the affected VFS target, final-ELF entry/scheduler mutation audits,
existing container probes, nine native regressions and Mousepad passed. Actual
probe/provider bytes were hash-checked in the guest. All jobs/groups finished;
heap, checked stop, original wait and four offline filesystem checks passed.
Astra reviewed inode identity, permissions and ownership without a blocker.

Evidence: `build/workloads/socket-bind-v2-20261023/completion-audit.json`.
Session: `6f32a1f55c1f439287ef3b8549070584`.
ABI-45 kernel: `3ebb70ce2dd3ce1ad534473d09524f352764df31c621cab9721017578eef1ef3`.

Unchanged SteamRT passes its read-only Wayland and session-bus socket binds and
reaches its final chdir. That fails because pressure-vessel did not export the
working directory: its O_PATH fstatfs call returns ENOSYS in Brook. Full stderr
is preserved and hash-verified. Runtime-child execution is not claimed here.
