# Retained regular-file binds

Regular-file bind mounts retain the exact source object and the covered target
entry. Source and target are walked in one VFS mutation transaction. Opening a
bind continues to use its retained inode after source rename, replacement or
unlink; no diagnostic pathname is used to reopen it.

File targets retain a parent and leaf for live reporting and recursive-bind
ancestry. Namespace copying, recursive binds, stacking, detach and rollback
preserve or release those references. Proc-FD reporting reconstructs a file
mount's visible entry from that retained parent, including target ancestor
renames, without requiring backend-wide regular-file name searches. All retained
opens, including Ext2 O_PATH regular files, keep an object reference for proc-FD
lookup; acquiring it under the FD lock requires no backend callback.

Source rename updates mountinfo's physical root. Unlink and rename replacement
mark the old source deleted. Its historical name is frozen after deletion;
this is bounded reporting rather than Linux's fully dynamic deleted-dentry
ancestry. Namespace and recursive-bind copies preserve that state.

The supported initial subset is regular-to-regular with one link on both
operands. This avoids alias ambiguity while mount crossing is inode-based.
Hard-link creation remains unsupported. An already validated file-mount root
can be stacked or rebound after its source is unlinked. Mixed file/directory
binds return ENOTDIR; special-file binds remain unsupported. Directory bind
behavior and existing mount restrictions are preserved. Module ABI stays 45.

Portable tests cover original target FDs, source rename/unlink, stacks, target
ancestor rename, recursive directory binds containing stacked file mounts,
read-only remounts, namespace-copy isolation and retained FDs after detach.
Both ordinary and O_PATH proc-FD paths are compared with Linux on tmpfs and
disk-backed files. Host tests additionally exercise
ambiguous aliases, allocation rollback, rename replacement in both retained
and legacy APIs, copied deletion state and full allocation/mount cleanup.

Linux controls passed 187 fixture checks plus five parent checks on each
filesystem. Brook passed 173 plus five on each. All 101 host tests, both affected
ASAN/UBSan targets, final-ELF entry/scheduler mutation audits, existing native
container probes, nine native regressions and Mousepad passed. All jobs/groups
finished; heap, checked stop, original controller wait and four offline
filesystem checks passed. Astra reviewed ownership and replacement handling.

Evidence: `build/workloads/regular-file-bind-v3-20261023/completion-audit.json`.
Session: `8153643374e34b5db99bb316dd07d087`.
ABI-45 kernel: `db4ca919962f93ea0c8e0d71dfc799957f11dbc369236469448623274e75357f`.

Unchanged SteamRT advances through regular-file mounts. The next failure is
its tmpfs mount on /newroot/var/pressure-vessel/ldso: the existing bounded
syscall rejects bubblewrap's mode option. Full stderr is preserved and
hash-verified. This checkpoint does not claim runtime-child execution or login.
Earlier diagnostic runs and their original identities remain separate evidence.
Detached file mounts can lose visible-entry reporting after detach; file data
access still uses the retained object, without a historical-name fallback.
