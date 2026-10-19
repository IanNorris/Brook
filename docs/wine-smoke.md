# Initial Wine64 smoke test — 18 October

Wine's Linux loader starts on Brook, but the first Windows console command is
blocked during wineserver setup. This is not Windows application acceptance.
No GUI, graphics, Vulkan, Proton, games or 32-bit executables were tested.

The pinned Nix package is Wine64 11.0:
`/nix/store/bk0lngs3cskihb07w5a2hlv6h2yvl0fc-wine64-11.0`.
Its 361-path closure was exported to an ignored local cache and offered to the
guest's existing `nix-install`, using private RAM-backed copies of the checked
prepared bootstrap disks. Configuration: eight vCPUs, 4 GiB, CPU composition,
headless, no Wayland service, and silent host audio. The candidate kernel SHA256
was `9e64f2783804714a8998f0bdaf2cbe756980fa0e668b98d07c38292615646ccd`;
it also contained the pipe-read ordering fix, whose source hashes are
retained in the audit. Main was not changed.

## Results

- Package download and unpacking reached the Wine payload, whose unpacker
  reported 429,309,728 bytes and exit zero. Installation then failed while
  publishing profile links: `Error: incomplete profile links: Not a directory.`
  Normal package installation therefore did **not** pass; registration may
  already have been published. Preserve this image for recovery diagnosis.
- Using the explicit package path independently of profile links,
  `bin/wine --version` exited zero and printed `wine-11.0` as UID1000.
- `bin/wine cmd /c echo BROOK_WINE_CONSOLE_PASS` exited one with
  `wineserver: /tmp/.wine-1000 is not owned by you`. The expected console marker
  was absent. A Brook-owned prefix directory had been created first.
- A separate boot verified a private ext2 temporary directory was owned by
  UID1000 with mode0700, then retried with `TMPDIR` pointing there. Wineserver
  still rejected `/tmp/.wine-1000`; the Windows command again exited one.

Both console attempts used `WINEARCH=win64`,
`WINEPREFIX=/data/wine-smoke/prefix`, `HOME=/data/wine-smoke`, empty display
variables and `WINEDLLOVERRIDES=mscoree,mshtml=` to avoid optional runtime
installation dialogs. The second additionally used
`TMPDIR=/data/wine-smoke/tmp`. These are diagnostic launch settings, not a
normal desktop launcher or an isolation guarantee.

## Next steps and evidence

First diagnose the ownership/metadata and path handling behind the server
directory rejection, using a rootless temporary-directory regression. Do not
assume ordinary Wine application syscalls are already working: the test stopped
before its first successful Windows command. The small installer repair below
addresses the encountered profile-link failure. Resume with
the same small console command before attempting a graphical Windows program.
Vulkan remains a separate planned effort.

Evidence is under `build/workloads/wine-smoke-20261018/review`, including
`install`, `wine-run`, `wine-run-ownedtmp`, exact command outputs and
`../smoke-audit.json`. All six tracked commands reached their expected terminal
statuses, drained original/full lineages and both output streams without loss.
All three sessions completed orderly sync/shutdown with original launcher and
QEMU exit zero, zero deferred cleanup, passing heap checks, no visible user
faults, and nine passing read-only post-stop filesystem checks. Private images
were retained and RAM copies released. The package-install and Windows-command
failures are negative evidence, not passing workload acceptance.

## Installer recovery verified

Wine's closure includes standalone file outputs, including the Mono MSI.
The installer tried to open `<file-output>/lib` while linking closure libraries,
then treated the resulting `ENOTDIR` as a damaged profile. It now checks the
store output's type before looking for bin/lib/desktop directories. File
outputs have no such directories; an actual malformed `lib` inside a directory
output still fails instead of being silently ignored.

Two new positive host regressions reproduced the old failure: a file/symlink
dependency and a standalone file package. A negative regression preserves
errors for malformed library directories. All 31 installer tests passed,
including existing publication, repair and cancellation checks.

A candidate static installer (`/boot/BIN/NIXWTEST`, not the old bundled helper)
was run on fresh private clones of the failed installation. Both recovery and
an immediate repeated install exited zero. `/nix/profile/bin/wine --version`
then passed as Brook, and passed again after cold boot. Full child logs were
retained as files and matched their reported lengths/SHA256 after shutdown;
the Mono MSI's contents also matched the original host store output. Both
accepted sessions drained all tasks and deferred cleanup, passed heap checks,
and completed orderly shutdown with original launcher/QEMU exit zero and six
passing post-stop filesystem checks. RAM copies were released.

Evidence: `build/workloads/wine-profile-repair-verified-20261018/acceptance-audit.json`.
The first diagnostic recovery returned zero but overflowed bridge output; it
is retained separately and excluded from acceptance. The bundled installer in
the earlier smoke image remains old; future tool/image builds pick up this
source fix. This verifies recovery of an already-unpacked package, not a new
fresh download through the candidate. Wineserver's temporary-directory
ownership rejection and Windows command acceptance remain unresolved. Further
Wine/Proton work is deferred while application stability testing continues.

## Unix temporary disk follow-up

On 19 October, the bounded Wine follow-up used kernel `ac42f03c`, the current
Wayland frontend, eight CPUs, 4 GiB RAM and software composition. Private copies
combined the checked Wine installation with checked application data; the
original images were preserved. A separate ext2 image mounted at `/tmp` through
the existing `BROOK.MNT` boot probe. No Wine binary or kernel check was changed.

Wine 11's server and client use `/tmp/.wine-UID`; this path is hard-coded in
the pinned [server](https://raw.githubusercontent.com/wine-mirror/wine/wine-11.0/server/request.c)
and [client](https://raw.githubusercontent.com/wine-mirror/wine/wine-11.0/dlls/ntdll/unix/server.c).
The earlier FAT-backed directory had no Unix ownership/mode metadata, explaining
why setting `TMPDIR` did not help. On ext2, a Brook-user probe verified UID1000,
GID100, mode0700 and matching path/FD inode identities for a new directory.
The mount root was UID0/GID0 and mode01777. Both Wine attempts passed the former
directory rejection and reached the next blocker.

`wine cmd /c echo BROOK_WINE_CONSOLE_PASS` and `wine notepad` both exited one,
reporting a running server they could not connect to. The console marker was
absent and no Notepad window appeared. Wine checks `lstat("socket")` before
connecting. Brook's bind/listen currently register a string in the Unix socket
registry without creating a filesystem socket inode. This prevents Wine from
discovering the listener. A separate rootless probe isolated the broader gap:

| Pathname socket check | Linux reference | Brook |
| --- | --- | --- |
| Bound node has socket type, caller UID and umask-derived mode | Pass | ENOENT |
| Relative connect reaches an absolute pathname bind in the same directory | Pass | ECONNREFUSED |
| Socket node remains after listener close, until unlink | Pass | ENOENT |

The exact probe is `scripts/workloads/unix_path_socket_check.py`; exit one is
negative evidence. These three cases do not cover the full socket namespace.
A complete implementation also needs ownership/permission enforcement,
symlink lookup, unlink/rebinding, duplicate binds and stale-node behavior.
That work is deferred beyond this small Wine pass. No Windows console or GUI
application acceptance, Vulkan or Proton support is established.

Wine servers outlived the failed main commands. Each was stopped using
`wineserver -k` with the same private prefix, then the original request was
polled again until its full lineage and output drained. All seven recorded
commands reached their expected terminal codes without missing output. Heap
integrity, orderly shutdown, owned launcher/QEMU exit zero and four offline
filesystem checks passed. All ten existing document fixtures retained their
content and ownership, including the latest Codium save.

Evidence: `build/workloads/wine-unix-tmp-20261019/candidate/acceptance-audit.json`,
the console/Notepad records, Linux and guest socket-probe results, and exact
tested helper sources. The initial console controller hit its observation
deadline while the Wine server remained alive; cleanup resumed the original
request rather than launching it again.

### Creating the temporary disk

The reusable helper creates a sparse 128 MiB ext2 image, verifies root ownership
and mode, runs an offline filesystem check and refuses to replace an existing
output:

```sh
python3 scripts/create_tmp_disk.py --output build/experiments/wine-tmp.img
```

Pass its absolute path as `BROOK_DATA_DISK` when launching a private experiment.
This uses the optional data/media drive slot, separately from the experiment's
`/data` and `/nix` disks. Contents persist across boots; this is a disk-backed
temporary directory, with no automatic boot cleanup. It does not implement
tmpfs or complete the broader temporary-filesystem work.
