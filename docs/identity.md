# Desktop identity and permission audit

Audit baseline: `811de61`, 17 October 2026. This records the implementation
before adding login or accounts. Brook currently provides a shared experimental
desktop; its UID support does not establish isolation between users.

## Current desktop

The shell launches applications as root by default. `run --uid N` sets real and
effective UID and GID to N. The default environment still says `USER=root` and
`LOGNAME=root`; `/etc/passwd` contains only root. The default HOME and all three
XDG state directories point into persistent `/data/home`. A new home is owned by
1000:100, matching existing experiment images, while the Firefox menu entry uses
UID/GID 1000. These are compatibility choices, not a consistent account model.
Existing homes retain their ownership at boot.

Root and UID 1000 each wrote, fsynced and reopened a 90,112-byte file under HOME
on an empty data volume, and verified it after a complete reboot. GIMP's generic
desktop entry also launched and saved settings there on the reboot. See the
17 October desktop acceptance notes in [workloads.md](workloads.md).

## Source findings

| Area | Current behavior and evidence | Required before accounts |
| --- | --- | --- |
| Identity queries | `sys_getuid/geteuid/getgid/getegid` return task fields. `sys_getresuid/getresgid` instead return zero for every result. | Track saved IDs and return consistent real/effective/saved values; test after launch, fork, exec and credential changes. |
| Credential changes | `sys_setuid/setgid` implement a simplified root/own-real-ID rule and update real/effective fields. No saved-ID fields exist in `Process`. | Define the supported Linux credential contract, including per-thread kernel credentials and libc's thread-group coordination; reject unsupported operations accurately. |
| Groups and capabilities | `getgroups` always reports supplementary group 0. `setgroups` and `capset` return success without recording changes. `capget` always writes a V3-sized zero result, without proper version/PID handling. | Store supplementary groups or return an explicit unsupported error; implement correct buffer validation and capability probes before allowing callers to rely on privilege drops. |
| Inheritance | Fork and thread creation copy `Process`, including UID/GID and umask. Shell creation explicitly supplies the default environment and optional UID override. | Verify credentials and environment together through fork/clone/exec, including failure paths. Define set-ID executable behavior separately. |
| File open | `Ext2FsOpen` checks existing-file read OR write mode for a nonroot effective UID/primary GID, unless the mount skips checks. O_RDWR does not require both bits. Creation skips this leaf check. | Require the appropriate access bits, check ancestor search and parent mutation permissions, preserve meaningful EACCES/EPERM errors and enforce mount policy. |
| Path traversal and mutation | Path resolution and parent lookup have no complete directory search/mutation authorization contract. Nix can explicitly skip permission checks. | Audit mkdir, create, unlink, rmdir, rename, symlink, hard links, sticky directories and directory-relative operations; restrict the writable Nix service deliberately. |
| Access checks | `AccessAt` validates flags and resolves paths, but implements existence rather than R/W/X permission evaluation, including AT_EACCESS. | Use real/effective credentials as requested and apply the same traversal rules as open/exec; cover descriptor and symlink variants. |
| Mode and ownership | Ext2 chmod checks effective owner/root; chown is root-only. New mount-aware ownership handling supports paths, descriptors, unlinked files, symlinks and dirfd variants. Ext2 ownership is currently 16-bit and rejects larger chown IDs. | Test mode/ownership through every mutation path and all filesystems. Resolve 32-bit ownership and accurate nonroot group-change rules before exposing account creation. |
| Runtime directory | Every shell application inherits `XDG_RUNTIME_DIR=/tmp`. Wayland and other IPC endpoints are shared. | Give a named desktop identity an owned, mode-0700 runtime directory with a defined lifetime; audit IPC peer credentials and access policy. |
| Persistent preferences | HOME/XDG is persistent, but root apps and compatibility-UID apps share it. Browser wrappers explicitly select their own profiles. | Choose one desktop UID/GID/name and matching environment, passwd/group entries and owned state. Preserve existing data during any migration; report conflicts rather than silently replacing ownership. |

Primary implementation locations: `src/kernel/src/process.h`, `process.cpp`,
`shell.cpp`, `syscall.cpp` (`AccessAt`, credential syscalls, chmod/chown), and
`ext2_vfs.cpp` (`Ext2FsOpen`, path resolution and inode mutation). The working
ownership regression is `scripts/workloads/ownership.py`; persistent storage is
checked by `scripts/workloads/home.py`. Neither test asserts full authorization.

## Delivery order and acceptance

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
