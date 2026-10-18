# App Gallery

App Gallery searches the on-disk Nix index, shows profile registrations and
provides install, repair and remove actions through the existing installer.
The GTK UI runs as Brook, uses native window chrome, loads the catalog in a
worker and filters in small UI-loop batches. Results are bounded to 200 rows;
the matching count tells the user when to refine the search. Installer output
streams into a bounded details panel, and completion reloads the manifest.
Packages installed locally but absent from the index remain discoverable.

![App Gallery after installing VLC on fresh disks](images/brook_app_gallery_1080.png)

The image is an unscaled native 1920×1080 completed-frame capture. It shows a
real install from the binary cache, rather than a mockup.

## Shared profile policy

The current desktop has one shared `/nix/profile`. Gallery allows mutations
only when running as UID 1000, invokes a fixed `/nix/bin/nix` executable with
separate action/package arguments, and never launches a shell or elevates UID.
Root or other-UID Gallery launches can browse but cannot start installs.
Read-only discovery reads the index and manifest directly. The installer
serializes its install/remove operations using a directory lock.

This uses Brook's existing writable Nix mount and does not establish an account
boundary: general directory authorization and the Nix permission bypass remain
under [audit](identity.md). A future protected system store needs a deliberately
authorized service after that enforcement exists. No root GUI, set-ID executable
or general privileged command endpoint has been added.

Installed status means registration in the installer's manifest. Preloaded
base-image components without manifest rows are not automatically counted or
made removable. Index availability does not establish Brook application
compatibility; actual workloads are tracked separately.

## Build and launch

```sh
nix-build tools/brook-gallery-pkg -o build/brook-gallery
python3 -m unittest discover -s tools/brook-gallery-pkg -p 'test_*.py' -v
```

Fresh experiment profiles include Gallery and its desktop entry.
`create_experiment_disks.py` copies the executable to `/nix/bin/brook-gallery`.
The legacy default package list and `update_nix_disk.sh --tools` also include it;
both legacy scripts now recompile the installer from source instead of
deploying a stale local binary. Run the updater inside nix-shell, against a stopped disk.

In Brook, use its installed App Gallery entry, or:

```
run /nix/profile/bin/brook-run /nix/bin/brook-gallery
```

Initial live tests staged the new package closure onto isolated existing disks.
Fresh data/Nix image deployment was subsequently checked below.

## Acceptance on 17 October 2026

The isolated eight-vCPU, 4 GiB VM loaded 20,992 unique catalog entries, including
its local compatibility entries. Native UI search selected `hello` 2.12.3;
Gallery fetched its closure from the binary cache and reported completion.
The resulting executable printed `Hello, world!`, and the manifest belonged
to UID 1000. Normal Gallery close returned the guest to ten tasks. Checked
shutdown and data/Nix/boot filesystem checks all passed.

Reboot preserved the package, executable and installed registration. An explicit
test lock made a repair request fail visibly without changing registration.
After removing only that test lock, native UI removal succeeded. The binary
shortcut and owned mutation lock were absent afterward; the launcher index was
refreshed. Gallery again closed normally, returned to ten tasks and passed
checked shutdown and all three offline filesystem checks. Sessions:

- Install: `73f571e7344b45e3975e464b62ba9dc6`.
- Reboot/removal: `fdf2682efbdb4987a6c5dbef716668f1`.

Five host tests exercise the actual production catalog parser, including the
repository index, blank separators, duplicate names, local registrations,
missing manifests and malformed input. The pinned GTK build passes
`-Wall -Wextra -Werror`. Ten separate installer tests and two-mount guest probes
cover [manifest preservation](../tools/nix-install/README.md). Evidence remains
under ignored `build/workloads/gallery-installer-20261017/`.

The debug TCP transport timed out during two observations; re-observing the same
VM recovered. No ambiguous install action was replayed. These are open bridge
reliability observations, not clean transport acceptance.

## Cooperative cancellation

The remote work branch includes cooperative Stop and close-and-cancel handling.
The installer requests cancellation through a private pipe; an active archive
finishes extraction before the fetcher stops. Completed store paths remain
cached, while cancellation before profile publication preserves registration.
Once profile publication starts, it finishes before accepting the request.
Metadata prefetch may also finish its current batch of up to 64 entries, so
Stop has no tight latency bound. This is not abrupt-kill recovery.

Seven additional host tests cover the actual installer/fetcher/extractor pipeline,
early requests across exec, closed standard input, cached retry and both sides
of the commit boundary.
An isolated guest run (`0d476c768ffd4076ac55cb565e0040b4`) verified Stop during
a dependency archive and closing Gallery during the final archive. Both left
complete cached data, unchanged registration and no mutation lock or helper
processes. Checked shutdown and all three offline filesystem checks passed.

Guest retry exposed missing kernel syscall 127 (`rt_sigpending`), used by the
new commit guard. The checkpoint at `a5e23fa` therefore refuses publication on
its original kernel. The follow-up implements the syscall and checks it with
the same 85-check probe on Linux and Brook: partial sizes, invalid destinations,
non-consuming snapshots, timer signals, fork clearing and thread isolation.
The new private pipe relocates both ends above standard streams and uses
close-on-exec; the fetcher fills closed standard slots before allocating archive
pipes, preventing a dup2/close collision when standard input is closed.

With the matched rebuilt kernel, session `21923ac1be4049e8b25b34bb515d4b00`
successfully retried the cached generated fixture as UID 1000, without starting
a cache server. Native Gallery Repair then completed and refreshed installed
state. Registration contained exactly one fixture row, the mutation lock was
absent, and closing Gallery returned to ten tasks. Checked shutdown and all
three filesystem checks passed. All 17 installer host tests and 26 hosted
suites passed. This fixture validates the operation path, not large-application
repair. Evidence remains under ignored `build/workloads/gallery-cancel-20261017/`.

The final kernel and descriptor setup were then exercised in session
`67caa20c55044a3997f8a0fe6b443923`. Stop during a gated dependency archive kept
the old three-package manifest, completed that dependency with its expected
SHA-256, skipped the root archive and left no fixture temporary directory or
mutation lock. The installer and fetcher both exited 130; Gallery remained
responsive with fourteen tasks including its workers.

A second attempt reused the dependency and gated the final archive. Closing
the native window showed “Stopping safely” and kept Gallery alive until that
archive finished. Gallery then closed, both complete cached files matched their
expected SHA-256, registration was unchanged, and ten tasks remained. With the
fixture server stopped, a subsequent UID-1000 install succeeded from those
cached files, adding exactly one fixture registration. The lock and temporary
directories were absent afterward. Checked shutdown and all three offline
filesystem checks passed. These controlled archive tests validate cooperative
cancellation, not SIGKILL/crash recovery or a maximum cancellation latency.

`rt_sigpending` observes the caller's blocked ordinary pending bits and matching
POSIX timers without consuming either. Ordinary process-directed signal routing
still lacks a shared group queue; blocked ignored signals and disposition-change
discarding also retain earlier limitations. The syscall does not claim to fix
those wider signal semantics. Its partial-size contract follows the
[Linux implementation](https://raw.githubusercontent.com/torvalds/linux/master/kernel/signal.c).

## Fresh disks and larger packages

On 17 October, the repository's `create_experiment_disks.py` built new 32 GiB
Nix and 8 GiB data images with a 481-path profile and current installer sources,
without game data. The acceptance boot disk and matched kernel/modules were
reused; this was not a rebuild of the entire boot disk. Session
`793aa3cdeed44930ad08184fe49b2b86` launched Gallery from its generated menu
entry, with its icon on the launcher, taskbar and native caption. The empty
manifest correctly reported zero registered packages; bundled components are
not yet registered as removable packages.

Gallery installed stock VLC 3.0.23-2 from the HTTPS cache. Store entries grew
from 481 to 851; completion was observed within 329 seconds (an upper bound,
not an exact timed benchmark). The executable reported its version. The stock
package lacks OSS options required by `brook-vlc`; its UI opened and closed
cleanly with explicit `--aout=dummy`. `--no-audio` alone still initializes
PulseAudio and hit the previously observed mutex-unlock abort. Registration
persisted across reboot. New shortcut requirements check the real VLC,
Ladybird and LibreOffice binaries as well as their wrappers; the rebooted menu
hid absent Ladybird while retaining installed applications.

A short 2-second CPU capture contained 1,613 samples across eight CPUs: 87.5%
idle, 4.4% in block-I/O leaf routines and 1.9% in TLB barriers. This is one
phase of the installation, not wall-time attribution. The profile and
unscaled captures remain under ignored `build/workloads/gallery-fresh-20261017/`.

Session `52efde0bc3d641e994de61a7d1068f93` then installed the existing
`vlc-brook-native` build through Gallery using its exported local cache and an
added index row. It played the attributed 720p Big Buck Bunny fixture on eight
vCPUs/4 GiB, with advancing video and nonzero captured OSS PCM. Pause produced
no active audio stream; resume restored nonzero PCM. Both manifest rows were
UID 1000 and the mutation lock was absent. Closing the player reproduced a
known worker page fault and left eleven VLC tasks blocked. After preserving
evidence, maintenance SIGKILL reclaimed the group. Both runs passed checked
shutdown and all three offline filesystem checks. Playback success does not
establish clean close or simultaneous-install stability.

The compatibility cache/index and video were supplied as explicit test inputs
after the initial fresh-image run; they are not automatically included by the
default disk script. Stock/native package file ownership and removal conflicts
remain part of the shared-profile work below.

## Per-link publication follow-up — 18 October

Binary, library and desktop links now replace via a staged symlink and atomic
rename, keeping the previous destination if staging or rename fails. Identical
targets are left in place. Link errors now make install/repair fail visibly;
metadata can already be committed, and the error explicitly requests a repair
retry. This is per-link publication, not whole-profile rollback.

All 28 installer host tests passed, including concurrent readers, injected
ENOSPC at symlink creation, real rename failure, parent flush failure and a
writer killed after staging. A killed writer's private temporary name remains
untrusted; retry does not delete it or mistake it for committed state. These
checks do not simulate an actually full disk or power loss.

Session `96e328f24f994c2eaf3aa1cc7185b4cf` tested a statically built installer
against an owned private cached profile as UID 1000. Normal install, no-op
repair, and binary/library/desktop publication failures followed by repair all
passed. No-op repair preserved link inodes; failed publication preserved the
blocking directory and returned an error. The fetch helper was a successful
no-op, so this does not renew large-package/network acceptance. The guest used
the checked ABI-18 kernel from the signal/desktop follow-up; no kernel rebuild
was needed. Heap, inactive audio, checked shutdown, launcher exit and all three
read-only filesystem checks passed. Evidence is under ignored
`build/workloads/gallery-links-20261018/`.

The initial probe used the wrong standalone CLI argument form; its failed
receipt is retained. Cleanup then exposed an independent kernel issue: saved
directory-descriptor paths silently truncate at 64 bytes. Absolute lookup of
the fixture desktop entry succeeded while dirfd-relative lookup returned
ENOENT. Python's fd-based recursive deletion consequently left entries behind.
The final publication probe uses absolute cleanup paths and does not claim to
fix this defect. The earlier quoting-error receipt and long-dirfd negative
control are also retained, with complete output and drained command lineages.

[Recovery design](app-gallery-recovery.md) records the remaining transaction,
shared ownership, base-package inventory and real disk-full failure matrix.

## Remaining work

- Disk-full and interrupted-install recovery across the whole profile. Atomic
  manifest publication alone does not transact symlinks and launcher metadata.
- Dependency/shared-file removal ownership and base-image package registration.
- Large-application repair/removal, icons for packages installed in the guest,
  automatic compatibility-package sources and broader fresh-image acceptance.
- Persisted desktop themes in GTK, richer app descriptions/icons, and broader
  real-application acceptance. Current styling is a first readable interface.
