# Brook package profile updates

`nix install`, `nix remove` and direct `nix-install` use one shared
`/nix/profile`. Install/remove operations acquire `.mutation-lock` with an atomic
mkdir before fetching or changing profile metadata/links. This avoids relying
on Brook's currently stubbed `flock`. A competing writer fails explicitly;
read-only discovery remains available. Direct `nix-fetch` and `nix shell`
closure downloads are not serialized by this profile lock.

Manifest add/remove now streams every row into a sibling temporary file,
flushes and fsyncs it, atomically renames it, then fsyncs the parent directory.
Removing a package no longer truncates the registration list at 256 entries.
Blank separators are preserved; malformed, torn or oversized rows cause an
error. A failed prepublication write preserves the old bytes. If the directory
flush fails after rename, publication has already happened: an error is not
proof that nothing changed. Removal changes links only after metadata commit.

Profile binary, library and desktop links now publish a sibling symlink with
`rename`, then synchronize the parent directory. The old destination remains
available if staging or rename fails. Identical targets retain their inode.
Link failures propagate to install/repair instead of producing a success
message; registration may already have been published, so an error explicitly
asks for a repair retry. A parent-directory flush error can likewise occur
after the new link becomes visible. Killed writers may leave hidden
`.brook-link.*` staging names; retries neither trust nor indiscriminately
remove those names.

This protects the manifest, not the entire profile as a transaction. Crashes
between metadata and symlink/launcher updates still need reconciliation. An
abruptly terminated writer may leave its lock; it is never automatically
reclaimed using a potentially reused PID. Its `owner` file is diagnostic.
Maintenance must establish that no writer remains before removing a stale lock.

Gallery uses SIGUSR1 for cooperative cancellation, with the signal blocked
across exec until the installer installs its handler. The installer closes a
private pipe to ask the fetcher to stop between complete archive operations;
it waits for the fetcher and its archive pipeline instead of killing writers.
Both pipe ends are above descriptors 0–2 and close-on-exec except the read end
explicitly passed to the fetcher. Closed standard streams are filled before
the fetcher constructs its archive pipelines.

Before publication, the installer blocks SIGUSR1 and queries `rt_sigpending`
to catch a request that arrived at the boundary. A request before that boundary
returns 130 without changing registration; a request after it lets publication
and link refresh finish. Use a kernel with syscall 127 implemented. Completed
store downloads remain cached. Metadata batches and active archive transfers
can delay cancellation; this protocol has no tight response-time guarantee.
It does not make forced termination or the complete profile crash-atomic.

Build and tests:

```sh
# Inside nix-shell:
$MUSL_CC -static -O2 -o build/nix-install tools/nix-install/nix-install.c
python3 -m unittest discover -s tools/nix-install -p 'test_*.py' -v
$MUSL_CC -static -O2 -o build/manifestcheck tools/nix-install/manifestcheck.c
```

Stage `manifestcheck` in an isolated guest, then run it as Brook with `/data`
and `/nix` as arguments. It creates and removes only its private fixture. On
17 October both mounts passed the 1,000-row preservation, add/remove,
missing-entry, torn-record and symlink-refusal checks. Session:
`fef41e76fb5c41c4940f497ecb0e9afa`. All ten host tests pass, including actual
failed writes using RLIMIT_FSIZE, concurrent readers, the public CLI with
1,000 rows, refusing an existing lock and releasing an owned lock after failure.

The [App Gallery acceptance](../../docs/app-gallery.md) exercised the actual
installer as UID 1000, package execution, reboot persistence and removal.
Seven further host tests cover cancellation in real installer/fetcher/extractor
processes using generated archives, closed stdin, early requests across exec,
cached retry, invalid control descriptors and both commit boundaries. The final
guest build passed gated Stop, close-and-cancel and offline cached retry;
cleanup returned to ten tasks and all three offline filesystem checks passed.

Desktop entry refresh redirects installed VLC through `brook-vlc`, preserving
upstream arguments, including absolute Nix-store and quoted executable paths.
Both the real executable and an executable compatibility wrapper must exist.
Absolute-path browser/editor entries use the same mapping. The collector's
private-filesystem regression is included in `test_desktop.py`; current
application playback/exit evidence is in
[application regressions](../../docs/application-regressions.md).

The recovery design and remaining acceptance matrix are documented in
[App Gallery recovery](../../docs/app-gallery-recovery.md).

Removal reports failed export cleanup, recognizes both absolute and relative
store links, and flushes changed directories. Registration is still published
first: an incomplete removal requires reinstalling to restore exports before
retrying. All 33 host tests pass, including real directory-permission failures;
the private UID1000 guest fixture also passes. See
[removal evidence](../../docs/app-gallery.md#removal-error-reporting--18-october-2026).
Whole-profile transactions, shared-provider restoration and real disk-full
recovery remain open.
