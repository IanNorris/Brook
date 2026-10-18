# Firefox, Chromium and VSCodium together — 2026-10-18

An isolated eight-vCPU, 4 GiB software-Wayland VM launched all three applications
through the normal app catalogue as Brook (UID 1000). The seed contained Firefox
150.0.1 and Chromium 148.0.7778.96. Stock VSCodium 1.112.01907 was installed through
the existing guest `nix-install` compatibility package path: 1.294 GiB of closure
in 116.2 host seconds. This was a fresh CLI installation, not App Gallery GUI,
interrupted-install or shared-file removal acceptance.

Firefox and Chromium both rendered Google. VSCodium accepted native keyboard
editing and Ctrl-S; an independent Brook-user command verified the exact saved
text, SHA-256, UID 1000, GID 100 and mode 0600. All three stayed open together.
First-window observations were 15.1, 13.7 and 8.5 seconds respectively; these
are not page-readiness measurements or matched performance comparisons.

![Firefox and Chromium test pages beside a saved VSCodium document](images/brook_browser_trio_1080.png)

Unmodified native 1920×1080 capture. Both browsers show a controlled synthetic
video fixture; this frame does not establish simultaneous video playback.
Window titles and app state can advance before the corresponding pixels paint.

## Audio and persistence

Each browser separately played the same 640×360, 24-fps VP9/48-kHz Opus fixture
while the other two applications remained open. Ten-second host PCM captures
contained approximately 441,000 stereo frames at 44.1 kHz, with no silent 10-ms
bins and an estimated 440-Hz tone matching the source. Firefox pause stopped
the stream; Chromium pause emitted zero samples on its still-active stream.
After all applications exited, capture returned no frames and an inactive
stream. No screenshots or CPU profiling interrupted the measurement intervals.

These checks establish audio delivery and cleanup, not glitch-free playback.
Firefox's reported media progress was approximately half real time in this
run; Chromium was closer to source rate. Delayed page-state delivery and the
known realtime-clock defect have not been separated from media scheduling.
The later CPU capture lost 24,279 records, so it cannot establish an application
CPU bottleneck or support a speedup claim.

A cold boot of the same private disks checked the saved document before any
application launched. Both browsers then opened read-only fixture URLs, with
no marker-writing arguments, and read their own distinct synthetic localStorage
markers. Startup windows were checked to reject a restored write-enabled page.
This proves persistence of this document and these synthetic site values;
real-site cookies and arbitrary profile data remain unverified.

## Exit and disk-clock correction

The first run drained all three complete UI launch lineages and shut down with
checked filesystem/device flush. All offline filesystems were clean, but its
serial log contained one recovered virtio-blk timeout. That run is rejected as
full stability acceptance; its immutable serial log remains preserved.

Inspection found that the block completion budget subtracted an RTC-adjustable
LAPIC tick count as an unsigned integer. A backward adjustment could appear as
a huge elapsed interval and immediately reset the queue. Completion waits now
use monotonic KVM pvclock when available; fallback subtraction rejects backward
intervals. The five-second budget and iteration cap remain. This does not repair
the global realtime clock or prove the cause of that particular timeout.

The corrected kernel, SHA-256
`9a9a374601cd0d7d7fc55e90c63a676cc4eec1488f0d804a22fa3628f9a7494c`,
passed the cold-persistence run, normal close of all three full launch lineages,
post-exit audio/heap checks, checked sync/stop, authoritative QEMU exit and
launcher exit zero, serial audit with no device/panic errors or user faults,
and read-only offline FAT and both ext2 checks. Profiles were warm on this
second run: fresh first-run timeout elimination is not yet established.

All 39 hosted suites and the supported compiled entry audit passed. Backward,
exact-deadline and wraparound interval cases are included; removing the guard
makes the regression checks fail. These unit checks cover elapsed arithmetic,
not emulated or physical device timeout injection. Full matching bootstrap
acceptance remains at `610592d`, before this block-clock change.

## Focused I/O acceptance

The same corrected kernel passed `io-clock-budget-home-fixture-20261018` in
20.7 host seconds on private host-RAM disks, eight vCPUs and 2 GiB guest RAM.
Eight workers wrote and read 64 MiB, checked boundary/fragmentation cases and
512 small files; the concurrent cache probe checked 64 MiB across eight
processes, and the inode-lifetime probe passed. A second boot verified persisted
contents and cleaned up. Both boots had checked shutdown and clean serial,
FAT and ext2 checks. Final free blocks/inodes exactly matched the starting
515,278/131,055. These RAM-backed measurements are not SSD throughput results
or a matched performance comparison.

The first invocation, `io-clock-budget-20261018`, failed the exact allocation
comparison because its empty data fixture omitted the five normal directories
created during home initialization. Read-only inspection of the stopped image
found precisely `home`, `.cache`, `.config`, `.local`, and `.local/share`.
The harness now includes these before its baseline; it still requires exact
allocation equality. The failed images and receipts remain preserved. This
harness observes legacy workload completion markers rather than tracked job
receipts; it does not replace full bootstrap or browser first-launch acceptance.

## Evidence and reusable exit barrier

Ignored evidence is under `build/workloads/browser-trio-20261018/`, including
`install-codium/command-evidence.json`, `verify-native-edit/command-evidence.json`,
`firefox-audio/`, `chromium-browser-audio/`, the original
`guest-final-serial.log`, and `reboot/`. The latter contains the cold document
receipt, read-only marker results, kernel manifest, final serial, native-close
barrier, checked stop and offline filesystem results.

`scripts/workloads/close_review_apps.py` closes known catalogue apps through
semantic window controls. It persists request identities before dispatch and
requires a verified boot identity, an untruncated task census, no target windows
and no tasks in any recorded launch lineage. Shutdown helpers in different
process groups remain part of that barrier. Six tests cover late helpers,
ambiguous close delivery, missing lineage, boot change, truncated census and
expired observations. It neither kills processes nor stops the VM. Checked
shutdown and offline filesystem checks remain separate requirements.
