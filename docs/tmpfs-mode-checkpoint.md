# Tmpfs root permission options

The bounded tmpfs mount interface now accepts `mode=` with octal permission
bits from 0000 through 07777. A missing or empty option string keeps the 01777
default. Repeated mode options use the last value. The complete string is
validated before instance allocation or mount publication, so rejected options
cannot cover the existing target. Unknown options, unsupported flags, malformed
numbers, values outside the supported permission range and empty comma-separated
items remain explicit errors. Size and ownership mount options are not added.

The backend creates its root with the requested permissions and the caller's
canonical UID/GID. The filesystem retains its existing user namespace,
NODEV|NOSUID flags and fixed instance/global resource limits. The root mode is
independent of the process umask. Existing callers retain the default mode.

Host tests cover every supported octal value, invalid input with unchanged
output, backend root ownership/permissions and rejection without allocation.
The portable Linux/Brook probe checks real mounts, defaults, repeated options,
root mode/ownership, tmpfs identity and restoration of the covered target after
detach. Brook-specific invalid-option checks verify the original target remains
unchanged.

This checkpoint also guards invalid retained reporting paths and checks the
object operation table together with the root inode identity. Detached file
mount reporting remains explicitly bounded as documented in the file-bind
checkpoint; retained data access does not use reported strings.

Linux passed 56 fixture checks plus five parent checks. Brook passed 76 plus
five, including invalid-option nonpublication. Both inherited file-bind probes
passed 173 plus five each on tmpfs and Ext2. All 101 host tests, the two affected
ASAN/UBSan targets, final-ELF entry/scheduler mutation audits, existing native
container probes, nine native regressions and Mousepad passed. All jobs/groups
finished; heap, checked stop, original wait and four offline filesystem checks
passed. Provider and both probe hashes were verified inside the guest. Astra
found no concrete parser, allocation or ownership blocker.

Evidence: `build/workloads/tmpfs-mode-20261023/completion-audit.json`.
Session: `a4c3833e74ac4526a2818638bd4133fc`.
ABI-45 kernel: `90dc3b051979f8dc713edd18a7dcd0d80673a40570104dce2edc9782bbef4bee`.

Unchanged SteamRT passes the tmpfs setup and reaches its Wayland socket bind.
The next failure is binding /oldroot/tmp/wayland-0 into the container, which the
regular-file-only subset rejects. Full stderr is preserved and hash-verified.
Runtime-child execution and Steam login are not claimed at this checkpoint.
