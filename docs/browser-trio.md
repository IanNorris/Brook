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
An initial half-speed inference from Firefox's receipt-time progress is
withdrawn: later measurements found delayed and out-of-order page-state
delivery. The original fixture lacked source timestamps, so its actual media
pacing remains unverified. A [source-timestamp follow-up](realtime-clock.md#firefox-follow-up-source-time-versus-http-receipt-time)
finds normal media progress per page second but many reported dropped frames;
the realtime clock defect has also been repaired separately.
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


## Permission-kernel refresh

The access-inquiry kernel was rechecked in two isolated eight-vCPU, 4-GiB
software-Wayland sessions. Kernel SHA-256:
`50fe3e571183c246225af8cbe724133394f8bdd3ce71a5afd7dbcdf997be368e`, with all
twelve frozen modules matching ABI 18. The host launcher/docs revision was
`36b482b7`; kernel source remains the accepted `37cccf60` checkpoint. These runs
used host-backed private clones of the accepted application disks, retaining
existing profiles. They are not fresh package/profile installation tests or
matched startup benchmarks. Speaker output was disabled throughout.

All three applications launched through the normal catalogue. The first
observed actual-app windows arrived after approximately 16.3 seconds for Firefox,
10.4 seconds for Chromium and 5.9 seconds for VSCodium. Launches were sequential;
each also had a separate 500-ms startup capture. The three captures contain
419/402/402 CPU samples and 3,122/3,209/3,734 context switches, respectively,
with zero producer drops and matching event counts. A separate concurrent-playback
capture retained 410 CPU samples and 8,343 switches without drops. These are
short windows, not whole-startup CPU-time estimates. Most timer samples hit idle
loops; executable-page-cache adoption appeared in the Chromium/Codium windows,
but the sample and IRQ-masking limits do not establish a bottleneck ranking.

VSCodium opened a new test file through native input, edited it and saved exact
bytes with UID:GID 1000:100 and mode 0600. Its SHA-256 is
`f04b724d98dbda2b8474f3b223204d7a4883fb8756fe7b18f43d335975fa23f9`.
The initial multiline `type` action lost line breaks and failed independent
byte verification. That terminal failure is retained as an input diagnostic.
Explicit per-line typing, Escape and settled Enter actions then saved the exact
intended three lines. The cause of the multiline-input failure has not been
isolated; general text delivery is not newly accepted by this follow-up.

### Audio and presentation measurements

Each browser played the controlled 640×360, 24-fps VP9/48-kHz Opus fixture while
the other browser and Codium stayed open. No screenshots or CPU captures occurred
during the ten-second PCM intervals. The host captured stereo output at 44.1 kHz.

| Single-browser playback | Firefox | Chromium |
| --- | ---: | ---: |
| Captured audio frames | 441,127 | 441,185 |
| Longest digital-silent interval | About 70 ms | No silent 10-ms bins |
| Media seconds per page second | 0.993 | 0.998 |
| Browser total video frames per page second | 23.91 | 23.97 |
| Video-frame callbacks per page second | 1.20 | 1.01 |

Firefox's seven silent 10-ms bins occurred at approximately 8.84–8.91 seconds
into its capture. Chromium's tone estimate was 440.3 Hz; Firefox's whole-capture
estimate was 436.8 Hz including the gap. Those estimates do not establish
sample-rate correctness or glitch-free playback. Pause yielded an inactive
zero-frame stream for Firefox and an active but entirely silent stream for
Chromium. After full application exit, both sessions had inactive zero-frame
output. Stream-active metadata describes the capture interval, not necessarily
its final instant.

The buffered source samples also confirmed a concurrent-playback round, with
about 13.2 seconds of overlap on the shared guest realtime clock. Firefox
advanced 0.552 media seconds per page second over its 19.655-second source
interval; Chromium advanced 0.994 over 17.101 seconds. Frame callbacks remained
low, about 1.17 and 0.82 per page second. Some Firefox samples reported hidden
while focused, including during its single-browser playback check. Browser
frame totals, callbacks and actual desktop updates remain separate evidence;
the screenshots sometimes retain earlier client pixels after titles or page
telemetry advance. This renews audio delivery and cleanup acceptance, not smooth
concurrent playback, compositor presentation rate or the visibility fix.

### Cold persistence and application cleanup

Before close, both browsers navigated to read-only fixture URLs. After checked
shutdown and a cold boot of the same private disks, read-only URLs without
marker arguments retrieved each browser's distinct synthetic localStorage
value. The Codium file's exact bytes and ownership were checked before launching
any application. All three then ran together again; untruncated task censuses
verified UID/EUID 1000 and GID/EGID 100 for their recorded non-kernel launch
lineages. This is synthetic storage/document persistence, not real-site cookies.

During that cold run, GIMP also launched normally and visibly reopened the saved
XCF and painted mark. After its full lineage exited, Writer launched and visibly
reopened the original ODT beside the three still-running applications. Its first
capture showed the initial empty page despite the updated title; a later native
capture displayed all three saved paragraphs. Document readiness was accepted
from the painted content, not the title alone. See the refreshed
[GIMP and Writer captures](application-regressions.md#gimp-and-document-persistence).

The review run drained all three complete UI launch lineages. The cold run
drained all five recorded lineages, including the already-closed GIMP. Both
passed post-exit audio and heap checks, checked filesystem/device sync, QEMU and
launcher exit zero, serial kernel/device/user-fault audits, and FAT plus both
ext2 read-only checks. Offline inode/data inspection verified UID:GID 1000:100
and unchanged hashes for the four original documents and the new Codium file.
Eight accepted tracked commands had terminal zero status, drained original/full
lineages, both EOFs and no gaps or dropped bytes. The separate failed multiline
input diagnostic is excluded from those eight passing receipts.

Evidence: `build/workloads/browser-permissions-20261018/acceptance-audit.json`,
with review session `1318aefd28c44f33ba50c840e8af1361` and cold session
`999282bb53c44ac89aba8191568a8817`. Profiles, audio, source batches, native input
intents and captures remain under `review/` and `cold/`. The loopback fixture
server exited zero after both VMs stopped. No kernel code changed for this
refresh; the [matching full bootstrap](bootstrap.md#40-suite-bootstrap-refresh-after-permission-checks-2026-10-18)
acceptance still applies. Forced-exit media tests, long-session workloads,
real-site persistence, browser presentation and complete identity isolation
retain their separate outstanding scope.

### Reusable fixture server

```sh
python3 scripts/workloads/browser_media_server.py \
  --output build/workloads/browser-check/fixture \
  --clip /absolute/path/to/controlled-clip.webm
```

Use a new directory below `build/`. The server binds host loopback on a free
port and records it, media/page hashes and PID in `servers.json`. Point the VM at
`http://10.0.2.2:PORT/?phase=NAME`; `write=1&marker=VALUE` explicitly seeds the
synthetic storage value, while `write=0` only reads it. Keep the same server
process/port alive across the cold boot so the origin stays constant. Reports
include source timestamps and arrive in a batch after Pause; HTTP receipt time
must not be used as video FPS. Touch `OUTPUT/stop-server` to shut it down. The
actual browser runs and a loopback GET/POST probe exercised this helper. It does
not start, stop or control any VM.
