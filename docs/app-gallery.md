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

## Remaining work

- Cancellation with safe subprocess cleanup; the UI currently stays open while
  an installer operation runs.
- Disk-full and interrupted-install recovery across the whole profile. Atomic
  manifest publication alone does not transact symlinks and launcher metadata.
- Dependency/shared-file removal ownership and base-image package registration.
- Successful repair, large-package installs, launcher icon/menu refresh and
  complete fresh-image/reboot acceptance through the Gallery.
- Persisted desktop themes in GTK, richer app descriptions/icons, and broader
  real-application acceptance. Current styling is a first readable interface.
