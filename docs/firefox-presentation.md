# Firefox presentation investigation — 2026-10-18

The epoch-anchor clock repair removes the demonstrated realtime regressions.
It does not establish a Firefox video speedup. This follow-up separates media
clock progress, browser frame counters, frame callbacks and VM display output.

## Buffered playback fixture

`scripts/workloads/browser_media_fixture.html` plays the same local 640×360,
24-fps VP9/48-kHz Opus clip. It buffers source-timestamped samples every 100 ms
and sends one batch after pause. Quiet mode does not update text during playback;
`status=1` updates a status block every 500 ms. This removes the earlier stream
of delayed/out-of-order HTTP telemetry requests from the playback interval.

Firefox launched normally as Brook in an eight-vCPU, 4 GiB software-Wayland VM,
using kernel SHA-256
`37cf304288e63f4525c9c50900a6b63c2de04c92e664483b8442dcc793b36a82`.
Four rounds used quiet/status/status/quiet order, an eight-second warm-up and a
ten-second host PCM capture. Analysis excludes the first eight seconds of page
samples. Its post-warm-up source intervals are 11.1–11.8 seconds, extending to
pause rather than exactly matching the PCM interval. A guarded screenshot
before the Pause input occurs after PCM capture but within that source interval;
these are diagnostic page-clock rates, not precise host presentation benchmarks.

| Round | Media seconds / page second | Browser total video frames / page second | Reported dropped frames | Video-frame callbacks / page second |
| --- | ---: | ---: | ---: | ---: |
| Quiet 1 | 1.000 | 24.00 | 0 | 1.36 |
| Status 1 | 1.001 | 24.06 | 0 | 1.17 |
| Status 2 | 1.002 | 23.95 | 0 | 1.12 |
| Quiet 2 | 1.003 | 24.01 | 0 | 1.11 |

All four PCM captures delivered audio and contained no digital-silent 10-ms bins.
A constant tone cannot establish glitch-free output or detect every repeated
buffer. Browser total-minus-dropped counts clearly do not independently prove
that the desktop displayed 24 different frames per second. Removing the status
text updates did not remove the callback discrepancy. Visibility/focus fields
were added for later diagnostic pages; the four original rounds did not record
them.

## Short, loss-checked profiles

Three separate 200-ms CPU/context-switch captures during quiet playback contain
485 CPU samples and 11,339 context-switch records. All producer drop counts are
zero and parsed event totals match their trailers. This avoids the overflow in
the earlier three-second capture.

420 samples land in `IdleLoop`, seven in userspace and 24 at unresolved module
addresses. This is insufficient to declare the system CPU-saturated or uniformly
idle: interrupt-masked time and tick-phase sampling can bias the capture.
Among stable Firefox launch-lineage TIDs, context-switch stacks include epoll,
futex, mapping validation and mutex acquisition. Counts are stack occurrences,
not CPU-time or blocked-time percentages. Only TIDs with the same incarnation
in the before/after census were attributed to Firefox. Further profiling must
separate event delivery, mapping-lock contention and actual presentation timing.

## Window-state discrepancy and activation correction

A later page reported `visibilityState=hidden` while the native WM reported its
Firefox window visible, focused and not minimized. A maximize/restore cycle
changed the page to visible, but video-frame callbacks remained approximately
1.8 per page second. Visibility alone is therefore not an established explanation
for the remaining presentation problem.

Inspection found a separate protocol gap: native focus events sent keyboard
enter/leave but no xdg-toplevel activation configure. Resizing unconditionally
advertised activation, and only for protocol versions 2 and newer, although
`activated` exists in version 1. The server now sends activation once per
native window transition, independently of how many seat resources the client
binds. Resize preserves that state.

The first candidate used a zero-size state-only configure and shrank Firefox
from 852×652 to 552×252. It was rejected and its stopped disks were retained.
The revision records requested window-geometry dimensions as pending state,
applies them on surface commit, and preserves the committed size when sending
activation. This avoids both natural-size shrinkage and treating CSD shadow
margins as extra client geometry. The real libwayland wire test checks version-1
activation/deactivation, one configure despite duplicate seat bindings, no
repeat configure on duplicate focus loss, and committed 800×600 geometry despite
an 852×652 buffer and a later uncommitted geometry request. Removing activation
or restoring zero-size configure makes the respective negative control fail.

The revised package is
`/nix/store/53wncg7ylk1az3j462kl61f66bzk9ph4-waylandd-brook-0.1-brook`.
Its cold Firefox window stayed 852×652. Firefox, Chromium and VSCodium launched
normally together; all observed non-kernel launch-lineage tasks used UID/EUID
1000. The saved document retained its exact bytes, UID 1000, GID 100 and mode
0600. All three full launch lineages drained on normal close. Post-exit audio
was inactive, heap integrity passed, checked sync/stop and authoritative QEMU
exit passed, the launcher returned zero, and serial/FAT/both ext2 checks passed.

**Cold Firefox still reported hidden with the revised server.** The cold
visibility assertion remains failed; the later three-app check is a separate
launch/identity/geometry smoke test, not a replacement passing visibility test.
The correction addresses activation and state-only sizing. General geometry
positioning/clipping, CSD margins in native resize/maximize events, browser
min/max buttons and the hidden-state/presentation issue remain open. Unmap/remap
and role-recreation behavior were not accepted in this check.

## Direct recording

A separate diagnostic recorded twelve seconds directly from the VM's private
RFB display, with no host desktop or microphone capture and no requested audio.
It finalized H.264 at native 1920×1080: 43 encoded frames, 33 source updates,
zero coalesced source updates and 12 missed encoder slots. Host encoder CPU time
was approximately 1.63 seconds; upstream guest/transport drops are unknown.
This is evidence of a low-rate display stream, not successful 30-fps acceptance.
There was one geometry, so display transitions, native SDL operation, long
captures and synchronized recording audio still need acceptance.

The MKV was remuxed without re-encoding to
`build/workloads/firefox-presentation-20261018/diagnostic-recording.mp4`:
43 frames, 1920×1080 H.264, 11.893-second container duration. This is a diagnostic
artifact; final showcase production has not begun.

## Evidence and host constraints

Ignored evidence roots:

- `build/workloads/firefox-presentation-20261018`: four batches/results, short
  profile exports and analysis, recording metadata, checked shutdown and fsck.
- `build/workloads/wayland-activation-20261018`: rejected zero-size candidate,
  cold visibility failure, original and revised package identities.
- Its `revised/`: preserved document/server identity, cold visibility failure,
  unchanged window size, three-app UID census, close barrier, checked stop,
  serial audit, filesystem checks and disk archive metadata.
- `build/workloads/wayland-activation-*-20261018.log` and
  `wayland-geometry-negative-*-20261018.log`: package builds and negative controls.

The package candidate ran on isolated host-RAM disk copies. FUSE mounting is not
available in this host environment, so offline deployment used debugfs to add
the canonical closure and replace `/nix/bin/waylandd`, matching the disk helper's
copy-based deployment. Canonical file/directory modes and launcher bytes were
verified. Package tools and app registrations were unchanged. This is not App
Gallery installation/recovery acceptance. The rejected disks were archived;
the final disk copy was hash-verified before releasing its temporary RAM storage.


## Permission-kernel follow-up

The [browser refresh](browser-trio.md#permission-kernel-refresh) on kernel
`50fe3e571183c246225af8cbe724133394f8bdd3ce71a5afd7dbcdf997be368e`
reproduced the distinction: single-browser media progressed near one second
per page second and reported about 24 video frames per page second, while
Firefox/Chromium frame callbacks remained about 1.20/1.01 per page second.
Firefox again reported hidden in some focused samples. Its ten-second PCM
capture contained an approximately 70-ms silent interval; Chromium's had no
silent 10-ms bins. These captures had no screenshot/profile activity during PCM.

In the source-timestamped concurrent round, Firefox progressed 0.552 media
seconds per page second while Chromium remained at 0.994. A separate 500-ms
runtime profile retained 410 CPU samples and 8,343 context switches without
producer drops. Short windows and IRQ-masking bias still prevent an unbiased
CPU/blocked-time attribution. Next diagnostics should connect Wayland callback
requests, timer dispatch, client commits and native display updates; neither
compositor-loop intervals nor browser frame totals prove actual video FPS.
