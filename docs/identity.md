# Desktop identity and permission audit

Initial audit baseline: `811de61`, updated 18 October 2026 with credential,
metadata and parent-mutation regression results. Brook currently provides a shared experimental
desktop; its UID support does not establish isolation between users.

## Current desktop

The shell and native Terminal launch applications as `brook` by default,
with UID 1000 and primary GID 100 (`users`). `run --uid 0` explicitly selects
root for maintenance. The named desktop identity has
matching new-home ownership and its synthetic passwd/group entries. Other UIDs
retain the numeric UID-as-GID default unless `--gid` is supplied. All three IDs
are set before ELF auxiliary-vector construction. USER/LOGNAME are root, brook,
or a numeric UID as appropriate; malformed, overflowing and sentinel IDs fail
before launch. `/etc/passwd` contains root and brook. HOME and all three XDG
state directories point into persistent `/data/home`.

Existing homes and preferences retain their ownership at boot. Root and brook
still share this home and runtime directory; named launch metadata does not
resolve mixed-owner application profiles or establish an account boundary.
The desktop default is implemented; legacy profile migration and the remaining
permission audit are separate from that policy.

Root and UID 1000 each wrote, fsynced and reopened a 90,112-byte file under HOME
on an empty data volume, and verified it after a complete reboot. GIMP's generic
desktop entry also launched and saved settings there on the reboot. See the
17 October desktop acceptance notes in [workloads.md](workloads.md).

## GIMP legacy profile migration

Existing root-owned GIMP settings can be migrated with
`scripts/migrate_desktop_profile.py`. Disk updates deploy it as
`/boot/SCRIPTS/MIGRATE_PROFILE.PY`; installed guest Python is required. Stop GIMP,
then use the kernel maintenance console or VM bridge (not ordinary Bash):

```
run --uid 0 /nix/profile/bin/python3 /boot/SCRIPTS/MIGRATE_PROFILE.PY
run --uid 0 /nix/profile/bin/python3 /boot/SCRIPTS/MIGRATE_PROFILE.PY --apply
```

The first command reports a plan. Apply requires root and refuses a running
GIMP, symlink maintenance paths, special entries or any profile UID other than
root/Brook. Missing or already-owned profiles are no-ops. The tool copies only
`.config/GIMP/3.0`, verifies its bytes, modes, ownership and mtimes, then retains
the original inodes under `/data/.brook-profile-backups/gimp-*/original` and
publishes a UID-1000:GID-100 working copy. Its GIMP parent directory also becomes
Brook-owned; the parent's former ownership and mode are recorded in the journal.
The original tree retains its ownership, modes, content and mtimes. Normal
rename ctime changes and read atime effects are not forensic preservation.

Copies use bounded buffers and do not follow profile symlinks. Original
hardlinks/xattrs remain with the retained original inodes; working copies retain
file bytes, modes, mtimes and link targets rather than hardlink relationships or
xattrs. No unrelated HOME files or old documents are recursively re-owned.
This is single-desktop maintenance, not an account/security boundary.

Files, journals and directory transitions are flushed. Copy failures leave the
original path intact; a failed second rename restores it. Interrupted cutover
leaves `gimp.lock/transaction` pointing to the retained journal and prevents a
second migration even when the original path is temporarily absent. Inspect
that transaction before recovery; the tool does not guess which tree to replace.
The regression's recovery only operates on its own private fixture.

This work exposed an ext2 rename defect: rewriting a moved directory's `..`
entry called the ordinary directory-touch helper, changing its mtime. The rename
now preserves that mtime while updating ctime; ordinary parent entry changes
still update parent times. This follows the separation in
[Linux ext2 rename](https://github.com/torvalds/linux/blob/master/fs/ext2/namei.c).
`scripts/workloads/rename_metadata.py` deliberately sets old timestamps so a
same-second rename cannot hide the error. All nine cases pass on Linux and the
fixed guest; the old guest fails both cross-parent directory cases. Those
historical checks ran as root; nonroot symlink creation ownership is covered
by the later full-width ownership checkpoint below.

`scripts/workloads/profile_migration.py` passes five guest groups: complete copy
and preservation/idempotence, failed-copy preservation, failed-cutover rollback,
interrupted-cutover refusal/recovery, and foreign-UID/symlink refusal. It checks
that injected failures actually reach the intended operation. The host's narrow
UID/GID mapping permits the symlink refusal case but skips four ownership cases;
those skips are not Linux ownership-comparison passes.

Kernel `c22675993ec5f98c18c7298cb27ed89c4dab6653e155840236a4f582bc69c83c`
passed 21 hosted suites and the guest regressions. Session
`a238069bad8c4ba89d8fd45383d352d3` migrated all 93 real GIMP profile entries.
GIMP launched as Brook, imported the transparent B image, saved a 789824-byte XCF
and exported a 286604-byte PNG. Both belong to 1000:100. The PNG's signature,
638x822 dimensions, chunk CRCs and compressed data were validated. The working
profile grew to 96 Brook-owned entries and persisted `(show-welcome-dialog no)`.
The original 93 entries still match their journal. GIMP quit normally, returning
to ten tasks with heap integrity OK; checked shutdown and all three offline
filesystem checks passed.

Reboot session `192dc0c1286e4c23a97dc131a8951d78` verified exact saved-image
hashes/ownership, the same preferences and original backup, and migration's
already-owned no-op. GIMP reopened the saved XCF without the welcome dialog or
profile permission errors, then quit normally again. The run again reached ten
tasks, heap integrity OK and three clean offline filesystem checks. Both runs
used eight vCPUs, 4 GiB and software rendering with speaker output disabled.
Evidence and the one-off image checker reside under
`build/workloads/gimp-user-20261017/`. The earlier failed timestamp check and
preserved original remain separately under `build/workloads/gimp-migration-20261017/`.
Optional plugin faults and dialog stacking/xdg_foreign support remain open.

## Source findings

| Area | Current behavior and evidence | Required before accounts |
| --- | --- | --- |
| Identity queries | All UID/GID queries return the real/effective/saved task credentials. ELF auxiliary vectors reflect launch/exec credentials and mark mismatched real/effective IDs with AT_SECURE. Required result pointers are validated. | Keep launch, fork, exec and credential-change regressions passing. |
| Credential changes | setuid/setgid, setreuid/setregid and setresuid/setresgid track saved IDs and validate the old tuple before changing it. Raw syscalls change the calling thread; libc coordinates changes with SIGSETXID. Root authorization currently means effective UID 0. | Capability-aware authorization and set-ID executable handling remain unsupported; do not describe this as full Linux credential support. |
| Groups and capabilities | Immutable, reference-counted supplementary lists are inherited by fork/clone and independently replaced; getgroups/setgroups validate buffers and the Linux 65536-entry limit. Tasks with all three UIDs nonzero support empty self capabilities; nonempty grants fail. Root/retained-root tuples return ENOSYS. Unsupported capability/security prctl options return EINVAL. | Ext2 existing-file opens, chmod setgid filtering and timestamp write checks use supplementary groups; other filesystem and operation paths remain to audit. Audit the remaining prctl options: several legacy options still return success without implementing behavior. Filesystem IDs are coupled to effective IDs; setfsuid/setfsgid are unsupported. |
| Inheritance | Fork and thread creation retain an owned group-list reference and snapshot all six IDs under the credential lock. Ordinary exec saves effective IDs and preserves real IDs/groups. Teardown releases the list, including failed creation paths. | Keep matching launch environment/passwd entries and verify repeated failure/cancellation cleanup. Define set-ID executable behavior separately. |
| File open | `Ext2FsOpenModeChecked` requires both access bits for O_RDWR and uses effective UID, primary GID and supplementary groups. Denials return EACCES without alias fallback; O_TRUNC requires write access. Creation skips the new leaf inode check but requires parent write/search; the explicit Nix bypass remains. | Keep checked component traversal and parent mutation regressions passing; audit other filesystems and define the writable Nix mount policy. |
| Path traversal and mutation | Ext2 checks parent write/search and sticky entry ownership under its write lock for creation, removal and both rename parents. A shared checked component walk preserves symlink/dotdot order and directory search checks through lookup/operation under a namespace read/write gate. Nix explicitly skips permission checks. | Complete pinned dirfd/CWD identity, executable authorization, other filesystem policies and hard-link policy; restrict the writable Nix service deliberately. |
| Access checks | Ext2 access/faccessat/faccessat2 evaluate all requested R/W/X bits, using real IDs by default and effective IDs with AT_EACCESS for both directory search and leaf metadata. Retained ext2 descriptors support AT_EMPTY_PATH, including unlinked files. Nix retains its explicit bypass. | Add other filesystem/non-vnode policies and pinned CWD/dirfds. Inquiry success does not authorize a later operation; executable loading still needs its own leaf execute check. |
| Mode and ownership | Ext2 chmod checks effective owner/root; chown is root-only. New mount-aware ownership handling supports paths, descriptors, unlinked files, symlinks and dirfd variants. Linux-layout ext2 ownership now preserves all 32 UID/GID bits through creation, stat, chown and existing leaf permission checks; new symlinks inherit creator credentials. | Test mode/ownership through every mutation path and all filesystems. Audit nonroot chown group-change rules, parent setgid inheritance and remaining filesystem operations before exposing account creation. |
| Runtime directory | Every shell application inherits `XDG_RUNTIME_DIR=/tmp`. This path is on the FAT-backed initrd, which cannot persist Unix ownership/modes. Wayland and other IPC endpoints are shared. | Give a named desktop identity an owned, mode-0700 runtime directory with a defined lifetime; audit IPC peer credentials and access policy. |
| Persistent preferences | HOME/XDG is persistent, but root apps and compatibility-UID apps share it. Browser wrappers explicitly select their own profiles. | Use the named desktop identity consistently in the default launcher and owned application state. Preserve existing data during any migration; report conflicts rather than silently replacing ownership. |

Primary implementation locations: `src/kernel/src/process.h`, `process.cpp`,
`shell.cpp`, `syscall.cpp` (`AccessAt`, credential syscalls, chmod/chown), and
`ext2_vfs.cpp` (`Ext2FsOpen`, path resolution and inode mutation). The working
ownership regression is `scripts/workloads/ownership.py`; persistent storage is
checked by `scripts/workloads/home.py`. Neither test asserts full authorization.

## Runtime evidence

The original audit in session `5be7052708c044be92e4ede7cae33442` found a
UID-1000 process reporting getresuid/getresgid `(0,0,0)` and supplementary `[0]`.
That contradiction is now covered by `scripts/workloads/identity.py`.

With kernel SHA-256 `fd332b684dcc883b6f28c795c673fe1f86df195d225367c5234ef5dfd2e29eab`,
session `b67d52f73e6a4f54b657bb50c37bf9e7` passed thirteen root-run groups:
query/auxv consistency, required buffers, thread-signal metadata, rejected
parent-TID pointers before thread allocation, temporary/permanent UID changes,
real/effective/saved permutation rules, supplementary-list validation and
inheritance, raw per-thread UID changes, libc pthread coordination, ordinary
exec, mismatched-ID secure exec and explicit unsupported capability errors.
A separate `run --uid 1000` passed the five nonprivileged groups; all UID/GID
queries and auxiliary entries reported 1000, with an empty supplementary list.
The 21 hosted test groups also passed, including the credential transition table.
Host Linux comparison passed query/buffer/thread-signal cases as an ordinary
user; privileged cases were skipped, not compared as root. Brook's rejected
parent-TID-pointer test is guest-only: Linux may publish the child despite an
unwritable parent-TID destination, so that test must not run on the host.

The libc test exposed two existing signal defects: tgkill silently dropped
SA_ONSTACK signals without an alternate stack, and delivery omitted SI_TKILL
sender metadata. It now falls back to the normal stack and preserves sender
TGID/real UID through delivery or sigtimedwait. Thread-directed lookup pins the
process and validates its TGID/TID before sending, including signal-zero probes.
This does not yet enforce cross-user signal permissions. Pending non-timer
signals still coalesce in the existing bitmask; this is not full realtime queue
semantics. Fork clears inherited pending signals. The original hung test and
fix are recorded under `build/workloads/identity-20261017` (local artifacts).

The supported transition rules follow the Linux
[setuid](https://man7.org/linux/man-pages/man2/setuid.2.html),
[setreuid](https://man7.org/linux/man-pages/man2/setreuid.2.html),
[setresuid](https://man7.org/linux/man-pages/man2/setresuid.2.html) and
[getgroups](https://man7.org/linux/man-pages/man2/getgroups.2.html) contracts
within the root-only privilege model above. libc's coordination is described by
its [pthread initialization](https://github.com/bminor/glibc/blob/master/nptl/pthread_create.c)
and [setxid handler](https://github.com/bminor/glibc/blob/master/nptl/nptl_setxid.c).
These checks establish credential consistency, not filesystem authorization or
multi-user isolation. Named desktop identity and permission enforcement remain
unfinished. GIMP renders and exits normally on the credential candidate. Codium's
standard launcher initially failed before renderer startup: Chromium's zygote
always drops capabilities using capset and fatally checks ENOSYS, even with
--no-sandbox. The --no-zygote direct-spawn attempt instead hit Electron's renderer root guard;
adding --disable-chromium-sandbox or ELECTRON_DISABLE_SANDBOX did not resolve
that diagnostic. A separate UID-1000 profile attempt stalled before mapping a
window; its stacks are saved locally as codium-nonroot-blocked-stacks.json.
Neither attempt is acceptance. Accepting an empty capset while retaining root
privileges is not a valid fix.

### Named identity and native editor follow-up

Kernel `9fd7b1acae6040ce33ca8bfe89cca1ab521126cfb3924d6482d6dbedc120d62c`
in session `cd6e269839314e00bf8ab5ebda1537ea` passed all fourteen root-run
probe groups and all six nonprivileged groups after reboot. The added group
verifies passwd/group lookup, USER/LOGNAME/HOME, primary GID and matching
stat/lstat/fstat/read lengths. Before the metadata fix, the guest reported
`NSS_SIZE_BEFORE 64 91 91` for pathname stat, descriptor stat and passwd bytes.
Pathname stat and lstat now use the exact passwd/group string lengths. Other
synthetic files' placeholder stat metadata remains a separate audit item.
Explicit `--uid 1000 --gid 123` returned UID `(1000,1000,1000)` and GID
`(123,123,123)`, with USER brook. Invalid negative, suffixed, overflowing and
sentinel launch IDs all returned no launched process. The 21 hosted groups
passed again, including parser boundaries and environment formatting.

With the named identity, a direct Codium launch in session
`dd0da1e66df545b3851f4b375824dfaf` rendered, accepted native typing and Ctrl+S,
and reopened the saved document through Ctrl+P. Independent Python reads
confirmed exact contents and owner 1000:100. Normal window close returned to
ten tasks with heap integrity OK. A checked shutdown and reboot preserved the
document and ownership; the rebooted native editor reopened it and again
closed normally to ten tasks with heap integrity OK. All three offline
filesystem checks returned zero after checked shutdown. Electron's Node mode
independently returned username
brook, UID 1000, GID 100 and home /data/home from os.userInfo().

This run used the installed Electron binary with its Nix wrapper environment,
--no-sandbox, --no-zygote, --disable-chromium-sandbox, --disable-gpu,
--disable-crash-reporter and --ozone-platform=wayland. Config, extensions,
state and temporary directories were fresh private codium-brook-test paths
under the persistent home. This is an accepted explicit nonroot configuration;
it does not prove the default root wrapper or mixed-owner existing profiles.
The effects of NSS correction and the changed profile/launch configuration have
not been isolated. The standard launcher integration is verified below;
rerun GIMP/browsers before accepting the whole desktop candidate.

### Standard Codium menu acceptance

Session `8e017fb410e44128a5e17051daaa8b6b`, with branding kernel
`3d458c5de54a597c964ed51059894987a3bb67117db24d10ec007e487c8e91c3`,
verified the deployed wrapper through the actual Applications menu. Both host
image discovery and the guest installer now generate
`--uid 1000 /nix/bin/brook-codium` when the package and wrapper exist. The wrapper
rejects root with an actionable launch command, uses the direct-spawn software
Wayland configuration, and creates separate `VSCodium-brook` profile/extensions
state and `codium-brook/tmp` beneath HOME. Existing root-run profiles are preserved.

The candidate already contained Codium's store closure but lacked its installed
desktop registration. Guest `nix-install brook-codium` registered that existing
package and refreshed the menu to thirteen entries. Clicking VSCodium rendered
the editor; native typing and Save As created
`/data/home/codium menu acceptance.txt`. An independent Python read confirmed
the exact three-line text and owner 1000:100. Closing the tab and reopening it
through Ctrl+P rendered the same text. Normal window close returned to ten tasks
with heap integrity OK. Checked shutdown and all three offline filesystem checks
passed. Evidence is in `build/workloads/identity-20261017/`, with native capture
`build/vm-bridge/8e017fb410e44128a5e17051daaa8b6b/artifacts/a91b7d4b0dea4e11b362606cd936d605.png`.
This menu integration was tested in one boot; the earlier explicit configuration
also passed reboot persistence. Extensions, debugger workflows, performance and
broader authorization enforcement remain unaccepted.

## Existing-file open authorization follow-up

Kernel `e9509a774719a2f47cb5cd256de84fd2835c05e6217bf319208eedd95020a198`
passed the new `scripts/workloads/permissions.py` matrix in isolated session
`6f29385055e9445a9a95915e9798b6e5`: 686 opens across fourteen modes and seven
identity/group combinations. The probe covers read-only, write-only, read/write,
O_CREAT on an existing file, and truncation with read-only, write-only and
read/write descriptors, requiring EACCES for denied
operations. It also checks that each identity can use the descriptor returned
when creating a mode-000 file. Owner bits take precedence over matching group
or other bits; mixed supplementary lists include an unsorted input and duplicate.

The pre-fix kernel reproduced both false O_RDWR success on write-only files and
wrong supplementary-group selection. Ordinary denied opens returned ENOENT.
Linux O_RDWR is now retained as a separate VFS flag, while existing WRITE callers
remain write-only. A checked filesystem-open callback returns ext2's EACCES from
under its inode lock, preventing legacy alias lookup after denial. Supplementary
membership and effective IDs are inspected under the credential lock. O_TRUNC
retains its write-access requirement before VFS performs the resize. These rules
follow the Linux [open contract](https://man7.org/linux/man-pages/man2/open.2.html).
All loadable filesystem drivers were rebuilt against the extended VFS interface.

On the patched kernel, all 21 hosted suites, all fourteen current root credential
probe groups and six nonroot groups passed. The ownership regression across
/data and /nix passed in session `494b7d655a3c4bf297f2c782821e82e9`. Codium launched
through Applications after reboot, reopened its persisted file, accepted native
edits and saved the independently verified result as UID 1000. Normal Alt+F4
close eventually returned to ten tasks with heap integrity OK. The first
injected text batch repeated a character during a stall; a second native edit
saved the intended text exactly. Input timing remains a separate issue to audit.
All three patched runs completed checked shutdown and all three offline filesystem
checks with zero errors. Logs, before/after matrices and the final summary are
under `build/workloads/identity-20261017/permissions-*.json`.

This is existing-file leaf authorization on the checked /data ext2 mount.
The [directory search rules](https://man7.org/linux/man-pages/man7/path_resolution.7.html),
parent mutation checks, sticky/setgid rules, access/faccessat, execution, other
filesystems and the explicit Nix permission bypass remain open audit items.
Accounts and multi-user isolation are not accepted by these results.

## Descriptor-relative paths and directory audit

The path review found independent syscall defects beneath the account work:
openat and several metadata/readlink callers could retry relative to CWD after
an invalid dirfd; the shared join silently truncated paths; and renameat sent
trusted kernel strings through the user-pointer validator, producing EFAULT.
The path join now pins the descriptor while copying its metadata, checks that
it names a directory, ignores dirfd for absolute paths and returns an explicit
errno. Every caller propagates that error. Relative open uses the same bounded
join, and an empty open path returns ENOENT. A separate DoRename helper accepts
trusted strings. Unsupported nonzero renameat2 flags return EINVAL rather than
claiming to honor NOREPLACE/EXCHANGE; flags zero uses the working rename path.
Mutation aliases under /boot are skipped when the whole prefixed path cannot fit.

The initial frozen-kernel guest probe recorded twenty mismatches in 31 cases,
including a successful create under a truncated filename. Its Python temporary
cleanup also failed: deep directory descriptors still retain only 62 path bytes.
The serial-derived baseline is `at-paths-before-serial.json`. Directory metadata
remains a path string, so this change does not repair deep dirfds or preserve
directory identity through rename, unlink or symlink replacement.

The final `scripts/workloads/at_paths.py` probe passed forty guest cases in
session `403dfeab8d7744f4816f30c6e84b6e1c`; thirty-nine host Linux reference cases
also passed. Brook adds its explicit bounded-join case. Controls cover successful
open/stat/statx, mkdir/rmdir, symlink, directory/CWD readlink, unlink, chmod,
timestamps and both zero-flag rename entry points, alongside invalid and
nondirectory descriptors. The readonly destination remains intact when unsupported
rename policies fail. All 21 hosted suites passed. The final controls run returned
to ten tasks with heap integrity OK, checked shutdown and three clean offline
filesystem checks. The initial malformed private fixture was removed by absolute
paths after preserving its evidence. Results: `at-paths-controls.json` and
`at-paths-result.json` under `build/workloads/identity-20261017/`.

The next traversal boundary must walk components in order, check search before
consuming each component (including dot/dotdot), expand symlinks before dotdot,
resolve mount crossings and preserve trailing slash requirements. It must return
structural and authorization errors directly rather than retrying the unresolved
spelling. VfsOpenExecutable has a useful component walk, but the general VFS
normalizer currently collapses dotdot first. sys_open also strips relative trailing
/.; chdir retains unresolved spelling. Parent mutations must check write plus
search under the filesystem write lock; sticky removal/replacement checks apply
to both relevant entries in a rename. A checked string followed by another lookup
also needs namespace synchronization; pinned directory objects are the longer-term
solution for CWD and dirfds.

`scripts/workloads/permission_audit.py` records these remaining gaps separately
from passing tests. In session `202f1504b8fb43cfbf8f2b708eefb2e3`, UID 1001:GID100
with no supplementary groups reproduced seventeen mismatches: seven traversal/type
cases, five readonly-parent mutations, both rename parents, two sticky-directory
operations and access-search. The public-directory creation control succeeded.
Each fixture is private under /data and the parent remains root. This is evidence
that directory authorization remains incomplete, not an accepted account boundary.
The same kernel passed 686 leaf-open checks, the two-mount ownership regression
and all fourteen current root credential groups. It returned to ten background
tasks with heap integrity OK, then passed checked shutdown and all three offline
filesystem checks. Kernel:
`ddc5239fb692a0baf443723ace56993c01a70c25d30f63af2cb739357bc81bb0`.

## Delivery order and acceptance

### Default desktop identity checkpoint (2026-10-17)

The kernel shell now defaults new app launches to Brook UID 1000:GID 100,
including bare executable commands. Native Terminal starts Bash with the same
identity and USER/LOGNAME. Maintenance commands that need root must explicitly
use `run --uid 0`. This changes the desktop default; it does not complete the
directory authorization audit above or introduce a login boundary.

Nonroot tasks whose real, effective and saved UIDs are all nonzero support
empty self capability queries and empty capset operations. Nonempty grants fail
with EPERM. V1/V2/V3 buffer sizes and version negotiation are checked separately.
Root or retained-root tuples still return ENOSYS; securebits, bounding/ambient
sets and other unsupported security controls are not implemented. This contract
depends on exec not granting set-ID or file capabilities.

`scripts/workloads/capabilities.py` passed five Linux comparison groups and nine
Brook groups (including retained-root cases). Session
`8de9d8f0f0694d3bbe967496527c984a` also passed all six nonroot identity groups
using the default launch, without a UID override. Kernel SHA256:
`9b3efe47b5b55d2d64761a73785be995b6df039b0b02adbe11c3880761204288`.
The candidate built successfully; hosted tests and shutdown/reboot acceptance
have not yet been repeated for this checkpoint.

Chromium rejects root launches and uses a separate chromium-brook profile,
preserving its previous root profile. Both Chromium and Codium retain their
normal zygote path. Chromium rendered Google's HTTPS consent page as Brook.
Launching Codium alongside it still produced ELF allocation failures and a
crashed editor window. Guest usage reached about 3.8 GB of 4.24 GB physical
memory. Concurrent browser/editor use is therefore not accepted. The same run
also logged a Chromium thread scheduler stall, which needs investigation.

Installer reliability remains a prerequisite for App Gallery: the candidate's
profile manifest contains an overwritten row after append-mode writes. Vnode
O_APPEND handling needs an atomic inode-level fix and regression coverage before
repairing registration. The existing Firefox package is indexed as
`firefox-stock`; its wrapper exists but its desktop registration is missing.
Neither browser reacceptance nor manifest repair is complete at this checkpoint.

Follow-up: [atomic append and installer acceptance](append-writes.md) records the
manifest fix, actual Firefox/Chromium registration, Brook-user Firefox HTTPS and
normal exit, and native Terminal identity verification. The earlier Chromium/
Codium memory failure and directory authorization gaps remain open.

1. Fix contradictory identity queries and false-success credential operations,
   with small Linux/guest comparison probes. Establish one named desktop identity
   and a matching primary group, environment and persistent directory layout.
2. Test and enforce file and directory authorization using private fixtures on
   `/data` and `/nix`. Cover allowed and denied operations as root, owner, primary
   group, supplementary group and unrelated UID; include O_RDWR, symlinks,
   dirfd, open-but-unlinked files, umask and concurrent credential changes.
3. Define the package install privilege boundary before App Gallery runs as the
   desktop identity. Keep read-only browsing/discovery separate from authorized
   store/profile mutation. Test failed installs, disk-full rollback and reboot.
4. Rerun GIMP, Firefox, Chromium, editors and installer workflows with the same
   identity. Verify saved settings and ownership after reboot, normal exit,
   targeted termination and offline filesystem checks.
5. Add login/accounts only after those checks prove the intended boundary. A
   login screen without that enforcement would not meet this audit's acceptance.

The audit identifies unfinished implementation, rather than accepting the
current desktop as a multi-user system. Filesystem integrity checks alone do not
prove authorization or identity correctness.


### Browser/editor acceptance follow-up (17 October)

The [memory and exit report](browser-memory-checkpoint-20261017.md) supersedes
the earlier browser/editor memory failure above: bounded immutable ELF sharing
allows native Chromium browsing and Codium edit/save/reopen together in a 4 GiB
VM. Fatal-signal group cancellation fixes retained Codium shutdown helpers in
three native-close cycles, including reboot persistence. The edited document
is owned by Brook. This progress does not resolve the directory authorization
or install privilege-boundary gaps; it is not acceptance of multiple accounts.


### Shared-profile App Gallery policy (17 October)

The first [App Gallery](app-gallery.md) runs as Brook and invokes the existing
installer without elevation. Its UI permits mutation only for UID 1000; other
identities can browse. The current Nix mount remains deliberately shared and
writable, with its existing permission bypass. This is the single-desktop
policy, not a protected store or authorization boundary. Install/remove
serialization and atomic manifest replacement passed host and guest tests.
General directory authorization must still be implemented before a protected
service or multiple accounts can be accepted.


### Full-width ext2 ownership checkpoint (18 October)

The old kernel truncated new file/directory owners to 16 bits and assigned new
symlinks to root. A frozen-kernel negative control reproduced both defects on
`/data` and `/nix`: creation by UID 66536:GID 131172 became 1000:100, while both
inline and block-backed symlinks became 0:0. Merely reporting full-width IDs in
stat would not repair these on-disk values.

The backend now reads and writes the Linux ext2 high UID/GID fields at inode
bytes 120/122, preserving other OS-dependent bytes. Creation, stat, chmod,
timestamps, chown and existing-file permission checks use the complete IDs.
New symlinks use the creator's effective credentials. Chown retains the `-1`
unchanged sentinel; final inode reclamation clears upper owner bits after open
unlinked references have drained. The layout and deletion convention follow
[Linux ext2's inode format](https://github.com/torvalds/linux/blob/master/fs/ext2/ext2.h)
and [inode serialization](https://github.com/torvalds/linux/blob/master/fs/ext2/inode.c).
This checkpoint covers Linux-layout images, not other creator-OS inode layouts.

`scripts/workloads/ownership_ids.py` uses private, explicitly named fixtures on
both ext2 mounts. Real fork/setgroups/setgid/setuid probes cover file/directory
and short/long symlink creation, owner metadata changes, path/descriptor chown,
upper-bit clearing when returning to low IDs, lchown without changing the target,
primary/supplementary groups and open-unlinked descriptors. Values up to UID
4294967294:GID 4294967293 survive stat and reboot. On `/data`, unrelated users
sharing the owner's low 16 bits are denied both chmod and file reads. The Nix
open-permission bypass remains deliberate and is explicitly excluded from
isolation acceptance.

Kernel SHA-256
`dd601109c01b5f782263491e84883b75d3229abafee971a63dd7bed1f7bf84b5`
passed the supported compiled entry audit and all 39 hosted suites. Session
`726d0a4464d041ee973da02403b6cd2d` passed the existing ownership regression and
the new preparation matrix on both mounts. Independent offline `debugfs` reads
verified fourteen inode owners, including the maximum IDs (printed as signed
-2/-3 by this e2fsprogs version and normalized to their uint32 values).
Cold session `64329a90e05f408da37f32e1cf1da19c` passed persistent owner/read checks,
metadata denials and supplementary-group reads. Four pre-existing Writer, GIMP
and OpenTTD documents retained their exact bytes and 1000:100 owners. Both runs
passed checked sync/stop, QEMU/launcher exit zero, serial fault/device audits
and read-only checks of the FAT and two ext2 volumes. Local evidence is under
`build/workloads/identity-uid32-20261018/`.

This does not complete directory traversal/mutation authorization, metadata
supplementary-group policy, parent setgid inheritance, Nix store protection or
IPC peer credentials. In particular, changing `XDG_RUNTIME_DIR` alone cannot
make the FAT-backed `/tmp` a private mode-0700 directory. A suitable runtime
filesystem and endpoint lifetime policy remain required; no login/accounts or
runtime-directory relocation are introduced here. The full two-path bootstrap
acceptance remains at `f4066d0`, before this kernel change, and has not yet been
renewed for this ownership checkpoint.


### Metadata permission checkpoint (18 October)

The full-width ownership kernel still lost setgid when an owner belonged to the
file's group only through the supplementary list. Timestamp updates also omitted
that list: a group writer was rejected, while a group member could incorrectly
fall through to the other-write bit. Both-now denials returned EPERM instead of
EACCES. The frozen-kernel probe recorded eight mismatches, including path,
descriptor and directory chmod and the incorrectly permitted timestamp update.

Ext2 metadata checks now consume the same locked credential snapshot as leaf
opens: effective root/owner status, primary or supplementary group membership,
and the selected access bits. Owners retain setgid for either membership form;
nonmembers have it stripped. Both-now timestamps permit owners or writers, with
EACCES for an unrelated nonwriter; explicit or mixed timestamps still require
owner/root. A matching group never falls back to other bits. These follow the
Linux [chmod](https://man7.org/linux/man-pages/man2/chmod.2.html) and
[utimensat](https://man7.org/linux/man-pages/man2/utimensat.2.html) permission rules
within Brook's existing root privilege model.

`scripts/workloads/metadata_permissions.py` checks high-ID owner/group cases,
path/fd/directory chmod, setgid retention/removal, primary/supplementary/other
write access, explicit/NOW/OMIT timestamps and unchanged mtime after denials.
The full matrix runs on private `/data` fixtures; the Nix timestamp bypass is
not presented as permission enforcement. The host's three owner-only controls
passed; a supplementary group was unmapped and skipped. Full cross-user Linux
comparison was unavailable because this host forbids privilege escalation.
No capabilities or concurrent credential-change stress coverage is claimed.

Candidate kernel
`a14bcc16b908acc5459cfabf4d65a204300d09d18e4f9e31b83efe9a3194ae52`
passed the metadata matrix, all 686 existing leaf-open checks, the two-mount
ownership suite and full-width creation/stat/permission/chown controls. Four
saved application documents retained their hashes and Brook ownership. All 39
hosted suites and the supported compiled entry audit passed before boot.
Baseline session `eed6a7df2e054980963f04553bdb72a1` and candidate session
`b6a271fc1c524fe189d0b49d0aa70759` both completed checked sync/stop, QEMU/launcher
exit zero, serial kernel/device/user-fault audits and three read-only offline
filesystem checks. Local evidence: `build/workloads/metadata-permissions-20261018/`.

Root-only chown, parent setgid inheritance, directory authorization, private
runtime directories and the writable Nix boundary remain unfinished. This
checkpoint does not renew browser, full-bootstrap or concurrent-use acceptance
for the new kernel.

### Parent mutation and sticky-directory checkpoint (18 October)

Ext2 now requires write **and** search access to the actual parent inode before
creating a file/directory/symlink or removing an entry. Rename checks both parents
and sticky ownership for the source and any replaced destination. Sticky removal
uses the entry's full-width owner, including for symlinks, rather than its
referent's owner. Cross-parent directory moves also require write access to the
moved directory for its `..` update; same-parent renames do not add that requirement.
All checks precede entry changes under the filesystem write lock. Root and the
explicit shared Nix bypass retain their existing policy. These checks follow the
[unlink](https://man7.org/linux/man-pages/man2/unlink.2.html) and
[rename](https://man7.org/linux/man-pages/man2/rename.2.html) rules within that
limited privilege model. Existing writable files can still be opened with O_CREAT
under a readonly parent; parent write is required for creation of a new entry.

The old kernel recorded 50 mismatches in 103 mutation controls. The first probe's
cleanup failed on the known shortened deep-dirfd path, so its nonzero receipt is
not acceptance. An absolute-path cleanup removed only that private fixture, then
the same VM completed a new negative-control request. The regression deliberately
uses absolute cleanup; it does not repair or accept deep directory descriptors.

`scripts/workloads/directory_mutations.py` now passes 122 operations plus 63
preservation assertions. Cases include primary/supplementary/other permissions,
owner-bit precedence, missing write/search, both rename parents, sticky source
and replacement ownership, symlink ownership, high-ID/low-bit collisions and
cross-parent directory write access. Denied operations preserve entry names,
inodes, owners, modes, link counts, payloads and parent mtimes. This is a private
`/data` matrix; full cross-user Linux comparison was unavailable on this host.

Preserving EPERM also required removing unlink/rename's legacy interpretation of
`-1` as a missing path. Only ENOENT may retry the `/boot` alias. FAT mutation
callbacks now use the existing typed error mapping while mkdir retains EEXIST.
FAT controls then exposed an existing guard that blocked every directory move or
removal whenever any FIL was open on that mount. A separate old-kernel run
reproduced EBUSY for an empty directory with an unrelated file held open. The guard
now scans such directories under the FAT lock and permits empty ones. Nonempty
directories retain the conservative open-file guard; descendant/open-directory
handle tracking and atomic replacement are not accepted here.
`scripts/workloads/fat_mutations.py` passes ten controls, including empty-directory
rename/removal with an unrelated open file, retained EBUSY protection for that
open file, replacement, EEXIST and ENOENT. The earlier failing FAT receipt remains
recorded; it is not counted as successful acceptance.

Reviewed kernel
`a63c9aaa2af15da78988b4efb49d7eebe9fdf243b7c6ab47c418cc7f5b7be17e`
passed all 39 hosted suites and the supported compiled entry audit. Session
`6b448be57cf24d8db3bef2c952a86d7e` passed the two new regressions, all 45 metadata
assertions, 686 leaf-open checks, two-mount ownership and full-width ID controls.
Four saved Writer/GIMP/OpenTTD documents retained their bytes and Brook ownership.
Checked sync/stop, QEMU/launcher exit zero, serial kernel/device/user-fault audit
and all three read-only offline filesystem checks passed. Evidence is local under
`build/workloads/directory-mutations-20261018/`, with final acceptance in `review/`.

The separate path audit still reports **eight** gaps: ancestor search for
open/stat/readlink, locked-directory dotdot, file/dotdot, symlink-before-dotdot,
absolute-symlink search and access-search. Its previous readonly-parent and sticky
cases now match. VFS still normalizes dotdot before expanding symlinks and may
retry an unresolved spelling; checked component traversal and explicit errors
must replace that behavior before account isolation is accepted. CWD/dirfd
identity, parent setgid inheritance, nonroot chown, private runtime directories
and Nix protection also remain open. This checkpoint does not renew full
bootstrap/browser acceptance for the new kernel.

### Shared component-walk checkpoint (18 October)

The VFS now uses one bounded iterative walker for open, executable lookup,
stat/lstat, readlink, metadata changes and mutation-parent resolution. It checks
search before consuming each component, including dot/dotdot, expands symlinks
before dotdot and preserves mount crossings and trailing-slash intent. Missing
and failed paths return explicit errors; callers do not retry an unresolved
spelling. These component rules follow
[Linux pathname resolution](https://man7.org/linux/man-pages/man7/path_resolution.7.html)
within Brook's existing effective-ID/root privilege model and path-size limits.

The namespace gate is now a reader/writer lock: lookups can share it, while
namespace, ownership and mode changes take it exclusively. The walk and backend
operation remain inside that gate. Mutation-parent resolution leaves the final
name to the backend so dot entries and final symlinks are not accidentally
converted into removal/rename of their referents. Ext2 supplies the optional
metadata permission policy, retaining the explicit Nix bypass. Other backends
need their own authorization policy; this is not an all-filesystem boundary.

Chdir stores the checked canonical spelling and fchdir checks final search
permission. This fixes physical CWD after a symlink and denies entry to an
unsearchable directory. Their retained paths are still strings, not pinned
namespace objects: rename/unlink/replacement and deep dirfds remain separate
work. Stat, statx, access and open syscall wrappers now preserve EACCES,
ENOTDIR, ELOOP and I/O errors; true ENOENT retains legacy `/boot` alias handling.
Leaf R/W/X access evaluation and real-ID access semantics remain unfinished.

Strict walking exposed a boot dependency: volume discovery registers `/mnt/probe`
without a physical `/mnt`. An absent ancestor of a registered mount is now a
virtual directory; an ordinary missing path, real nondirectory or I/O failure is
not replaced. The initial boot never discovered its data volumes or started the
bridge. Its forced, unsynced diagnostic stop is explicitly rejected acceptance,
even though its offline filesystems checked clean. The fixed boot completed the
ordinary acceptance gates.

A compatibility probe also caught EINVAL from following procfs FD entries as
textual symlinks. Procfs now explicitly marks those entries opaque to the walk,
preserving its prior stat/lstat/readlink behavior and syscall-specific FD hooks.
That behavior is not full Linux magic-link or target-metadata support. The new
VfsFsOps fields require module ABI **17**; all twelve shipped module headers were
checked and modules rebuilt together with the kernel.

The actual shared walker has a hosted model test with assertions enabled in
Release builds. All **40** hosted suites and the supported compiled-entry audit
pass. `scripts/workloads/path_traversal.py` passed 38 owner-level Linux controls
and 43 Brook controls, adding two mount-crossing and three proc-fd compatibility
cases. All eighteen cases in `permission_audit.py` now match; its previous eight
mismatches are closed. The 185 mutation/preservation assertions, ten FAT controls,
45 metadata assertions, 686 leaf-open checks and two-mount ownership/full-width
ID regressions also pass.

`scripts/workloads/path_lookup_race.py` exercised 400 reads during 200 atomic
symlink replacements on the reviewed kernel: 223 public payloads and 177 EACCES
results, with no protected bytes observed. This is a bounded contention test,
not proof of all namespace races or a performance benchmark.

Reviewed kernel SHA-256:
`424e461dd8d5e1855c4e1268904be43a69fe3714f0bd3b43b63e761cf2d38371`.
The local evidence root is `build/workloads/path-walk-20261018/`; `candidate/`
contains the rejected boot, intermediate runs retain their own kernel identities,
and `review/` holds the final ABI-17 regression and application run.

In that final run, GIMP's normal Brook-user catalogue launch reopened the saved
XCF and its painted mark, then drained its complete UI launch lineage on native
close. Four existing application document hashes remained unchanged; independent
offline inode inspection confirmed 1000:100 ownership. Checked sync/stop,
QEMU/launcher exit zero, serial kernel/device/user-fault audit and the FAT plus
both ext2 read-only filesystem checks passed. Command receipts are terminal with
no live task lineage, both EOFs and no output gaps. The rejected initial boot is
not included in those acceptance claims.

Account isolation remains unaccepted: real-ID and leaf access/execute policy,
pinned CWD/dirfds, synthetic/magic-link authorization, nonroot chown, parent
setgid inheritance, other filesystem policies, runtime directories and the
writable Nix boundary still need work. Browser concurrency, full bootstrap and
performance acceptance have not been renewed for this kernel. The earlier
39-suite bootstrap checkpoint stays pinned separately in the README.


### Access inquiry checkpoint (18 October)

`access`, `faccessat` and `faccessat2` now evaluate the requested leaf R/W/X
bits on enforced ext2 mounts. Both the component walk and leaf inquiry select
real UID/GID by default, or effective IDs with AT_EACCESS; supplementary groups
participate in either selection. Owner precedence and combined requests use the
same credential snapshot as other ext2 permission checks. Within Brook's
existing UID-zero privilege model, root still needs at least one execute bit
for regular-file X_OK. These rules follow
[Linux access inquiries](https://man7.org/linux/man-pages/man2/access.2.html);
capability-aware root behavior remains unsupported.

AT_SYMLINK_NOFOLLOW evaluates link metadata. AT_EMPTY_PATH on an ext2 vnode
uses the retained object rather than looking up its saved pathname, including
after unlink or when its parent is inaccessible. Descriptor queries therefore
need no parent search. Relative dirfd and empty-CWD queries still use stored
paths and are not pinned against rename/replacement. Non-vnode descriptors and
vnodes without a permission callback return EOPNOTSUPP for nonzero requested
modes; F_OK remains an existence inquiry. Backends without a pathname policy
retain their legacy behavior. The Nix mount deliberately bypasses both pathname
and retained-vnode permission inquiries. Synthetic compatibility fallbacks use
their declared metadata, but are not a complete synthetic/magic-link policy.

The added real-ID filesystem and retained-vnode callbacks require module ABI
**18**. All twelve frozen module headers match. `access_permissions.py` passed
**6,204** guest checks across twelve credential combinations, including primary
and supplementary groups, high IDs, differing real/effective IDs, ancestor
search, retained/unlinked descriptors, symlinks and error cases. The corrected
negative control on the previous kernel recorded **3,496** mismatches in that
same matrix. The unprivileged Linux owner subset passed **448** controls;
privileged Linux credential comparisons were unavailable on this host and are
not claimed. `access_flags.py` passed another **45** Brook controls for relative
dirfds, empty CWD, retained descriptors, explicit unsupported pipe permissions
and the shared-Nix bypass.

The existing 43 traversal, 185 mutation, ten FAT, 45 metadata and 686 leaf-open
checks passed, along with ownership/full-width-ID controls and all eighteen
remaining-path audit cases. The bounded namespace-contention probe returned
217 public reads and 183 EACCES results during 200 symlink switches, with no
protected payload in 400 reads. All **40** hosted suites and the supported
compiled-entry audit passed; this is not expanded interrupt-handler coverage.

Reviewed kernel SHA-256:
`50fe3e571183c246225af8cbe724133394f8bdd3ce71a5afd7dbcdf997be368e`.
Local evidence: `build/workloads/access-checks-20261018/acceptance-audit.json`,
review session `ff61cd9f5f72410281d4077fbc7f9801`. Sixteen tracked commands
finished with zero exit, drained original/full task lineages, both EOFs and no
output gaps. GIMP's normal Brook-user catalogue launch displayed its Welcome
window and recent-document list, then native close drained its full launch
lineage. This run did not reopen the XCF. Independent offline inspection after
shutdown verified all four saved document hashes and UID:GID 1000:100. Checked
sync/stop, QEMU/launcher exit zero, serial kernel/device/user-fault checks and
three read-only filesystem checks passed. These test disks are host-backed
isolated clones, not RAM disks.

Executable authorization is separate: access(X_OK) success must never authorize
a later exec. Actual executable leaf checks, pinned CWD/dirfds, nonroot chown,
parent setgid inheritance, runtime directories, other filesystem policies and
Nix protection remain open. Both full bootstrap paths subsequently passed at
this source revision, as recorded in the [40-suite refresh](bootstrap.md#40-suite-bootstrap-refresh-after-permission-checks-2026-10-18).
The subsequent [application refresh](browser-trio.md#permission-kernel-refresh)
renewed controlled three-app concurrency, synthetic storage/document persistence,
GIMP/Writer reopen and full-lineage close on this same kernel. It also retained
the measured audio/presentation and native-input limits; real-site cookies and
complete identity isolation remain unaccepted. Login/accounts remain deferred.


The subsequent [retained-directory follow-up](dirfd-identity.md) fixes silent
64-byte descriptor-prefix truncation and adds sequential rename/replacement
lookup through ext2's retained inode. Its ABI-19 layout requires all modules to
be rebuilt. Atomic namespace-relative operations, retained process CWD and the
other identity gaps above remain open; this does not complete account isolation.


The [executable-permission follow-up](exec-permissions.md) now checks execute
intent on ext2 data files and preflights ELF interpreters before exec commits.
Its 672-case matrix covers effective IDs, groups, execute-only files and denied
interpreter calls preserving the old process. The Nix bypass, dispatch-time
launch credential context, set-ID/capability policy and remaining namespace
limits remain explicit. This does not finish the accounts boundary.
