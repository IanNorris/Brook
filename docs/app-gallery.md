# App Gallery

App Gallery searches the on-disk Nix index, shows profile registrations and
provides install, repair and remove actions through the existing installer.
The GTK UI runs as Brook, uses native window chrome, loads the catalog in a
worker and filters in small UI-loop batches. Results are bounded to 200 rows;
the matching count tells the user when to refine the search. Installer output
streams into a bounded details panel, and completion reloads the manifest.
Packages installed locally but absent from the index remain discoverable.

![App Gallery after installing hello](images/brook_app_gallery_1080.png)

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

The current live tests staged the new package closure onto isolated existing
disks. Full fresh-image menu/icon deployment remains to be exercised.

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

## Cancellation checkpoint (work in progress)

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
repair. Repeat gated Stop/close acceptance with the final descriptor setup
before treating cancellation as fully accepted. Evidence remains under ignored
`build/workloads/gallery-cancel-20261017/`.

`rt_sigpending` observes the caller's blocked ordinary pending bits and matching
POSIX timers without consuming either. Ordinary process-directed signal routing
still lacks a shared group queue; blocked ignored signals and disposition-change
discarding also retain earlier limitations. The syscall does not claim to fix
those wider signal semantics. Its partial-size contract follows the
[Linux implementation](https://raw.githubusercontent.com/torvalds/linux/master/kernel/signal.c).

## Remaining work

- Repeat gated cancellation acceptance with the final descriptor setup.
- Disk-full and interrupted-install recovery across the whole profile. Atomic
  manifest publication alone does not transact symlinks and launcher metadata.
- Dependency/shared-file removal ownership and base-image package registration.
- Successful repair, large-package installs, launcher icon/menu refresh and
  complete fresh-image/reboot acceptance through the Gallery.
- Persisted desktop themes in GTK, richer app descriptions/icons, and broader
  real-application acceptance. Current styling is a first readable interface.
