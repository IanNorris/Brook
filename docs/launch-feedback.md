# Launch feedback checkpoint — 17 October 2026

Selecting a launcher entry immediately creates a pending taskbar entry with the
app icon and animated progress dots. Hovering shows `Starting: <app>`. A worker
loads the executable and runs the shortcut; this work no longer runs on the
compositor thread. The launcher's existing maintenance worker consumes up to
eight outstanding requests. Identical pending identities coalesce.

![Chromium pending launch at native 1920×1080](images/brook_launch_feedback_1080.png)

Native windows match an inherited launch token. Wayland windows match the exact
app ID or executable identity from launcher metadata; matching a new window is
allowed once dispatch has started. A pre-existing single-instance app can resolve
the request after successful dispatch. This is evidence of an application window,
not proof that the newly requested process completed successfully. Popups do not
resolve pending entries.

Failed dispatch and a completed process tree without a matching window produce
an exclamation badge and `Launch failed` hover text. A five-minute deadline
changes a pending entry to `Still waiting`; it does not cancel the loader or app.
Notices expire after 15 seconds or can be dismissed by clicking them. Clicking a
busy entry does not cancel its process. If all eight slots are occupied, the
launcher stays open with `Launch queue full` in its header.

Requests own their command, metadata and icon pixels, so launcher refreshes do
not pin old snapshots while an app starts. Shell dispatch is serialized across
launcher, bridge, interactive and nested-script callers. Nested scripts preserve
script mode and propagate errors. Child CWD, trace flags, quiet descriptors and
launch token are set before scheduler publication; the shell returns a captured
PID and never dereferences a potentially reaped child after publication.

## Validation

All 24 hosted suites and the two-pass release kernel build pass. The launch state
suite covers duplicate requests (including legacy shortcuts without IDs), stale
completion after slot reuse, bounded capacity, timeout, native identity precedence,
new versus existing Wayland windows, and failed dispatch. An unrelated new window
cannot erase a request that is still queued, and an existing window cannot hide
a failed dispatch.

Isolated 8-core, 4 GiB CPU-compositor VM sessions:

- `c9d8bc3814324015bff3361e61e4a7f3`: Chromium pending icon and hover label,
  replacement by its real window entry, opening the launcher during Codium
  startup, both apps rendering and closing through native controls, and a
  deliberately broken shortcut producing a visible failure notice. After closure:
  10 tasks, 270,708,736 physical bytes used. Checked shutdown and all three
  filesystem checks passed. This candidate precedes the final legacy-ID duplicate
  fix; the screenshot above is from this session.
- `87bc4c2777f346939bdd898d8f52f020`: final kernel SHA-256
  `c7614e262609a35f67f6b69a775c13398f485fe39e9d3892a3ef4c8733b66ef5`.
  Two quick clicks on a 20-second sleep shortcut produced exactly one dispatch
  and one child. A failing shortcut showed a failure notice; paired captures
  6.6 seconds apart verified explicit dismissal before expiry. Another shortcut
  spawned 100 short-lived `true` children, then sourced the failing script;
  the outer dispatch reported failure without deadlock and the task count
  returned to baseline. Individual success exit statuses are not logged for
  these quiet children. Mousepad's pending entry
  resolved to its normal window entry and it closed natively. All temporary
  shortcuts were removed. Final sample: 10 tasks and 210,870,272 physical bytes
  used. Checked shutdown and all three filesystem checks passed.

Local evidence is under `build/workloads/launch-feedback-20261017/` and the two
session registries. Runtime disk images and fixture shortcuts are not committed.
These are bounded correctness checks, not a startup-performance benchmark.

## Remaining work

- Display scaling remains open. Busy cursor feedback was added in the follow-up
  described below.
- Older custom Wayland shortcuts need accurate app ID/executable metadata when
  not replaced by an installed desktop entry. Unknown IDs retain the pending
  entry until process exit or timeout. This is not complete launch correlation
  for every installed app.
- Recheck GPU composition, overflow interaction and full-queue UI in a VM;
  capacity and timeout are currently covered by hosted state tests.
- The native terminal toolbar button still creates its terminal directly;
  the worker change applies to launcher entries.


## Busy cursor follow-up

Pending launches now add an animated badge beside the existing cursor. The
client-provided pixels and hotspot remain the base image; the compositor takes
one synchronized snapshot and composes into a separate frame used by both CPU
drawing and GPU upload. Removing the badge restores the original cursor.

![Native-size custom cursor with busy badge](images/brook_busy_cursor.png)

Cursor uploads stage at most 16 KiB into kernel memory before taking the cursor
publication lock. Lazy pages are populated first; each read is then revalidated
under the address-space mutation lock so concurrent unmap/protect cannot retire
the source while it is copied. Source memory is never read under the cursor
spinlock. Publication is protected against fatal cancellation, including reset.

All 25 hosted suites pass. The shared composition suite verifies base pixels,
alpha and hotspot preservation, animation phases, restoration and 64×64 bounds.
The Brook-only `cursorcheck.c` guest probe covers invalid dimensions, null and
protected memory, an unaligned 16 KiB upload spanning five distinct mappings,
and 500 uploads racing 200 unmap/remap cycles. It also provides bounded `show`
and `reset` modes for live composition checks.

The initial CPU session `7708742c715b48c1860f38f39db9cef9` passed with 2 accepted
and 498 safely rejected racing uploads. Custom and default cursor busy badges
were visible; reset during startup preserved the badge. After startup ended the
badge disappeared, with 10 tasks remaining. Checked shutdown and all three disk
checks passed.

Initial GPU session `08d9d0c433034b1ab8c868f47131af52` confirmed GPU DRAW composition
active on the host render node and passed with 7 accepted and 493 rejected racing
uploads. It verified custom cursor/reset while busy, edge movement and Mousepad
launch/close. The screenshot above is from this session. Checked shutdown and
all three disk checks passed. Its last 12-task sample included maintenance
processes and is not a post-cleanup baseline measurement.

Local evidence is under `build/workloads/busy-cursor-20261017/`. Cursor tests do
not establish arbitrary syscall user-buffer safety elsewhere in the kernel.

The final cancellation-protected candidate was rerun as GPU session
`d10ec83733fd42518f99569522c6f3db`, kernel SHA-256
`855b0d0d22af04b3658cdf04febd571e37eb1c0f1e495a1349fa44800ac73c65`.
The probe passed with 10 accepted and 490 rejected racing uploads, zero
failures; the final sample after fixture removal returned to 10 tasks and
212,443,136 physical bytes used. Checked shutdown and all three filesystem
checks passed. The earlier visual CPU/GPU checks use the same composition code;
the final change additionally protects publication/reset from fatal cancellation.

This last boot exposed a host readiness race: session registration can precede
creation of the serial log. The helper now waits for that file instead of
aborting. The already-running VM was reattached and tested, not restarted.
The launch helper also accepts explicit GPU/composition parameters for these
parity checks; its bootstrap defaults remain CPU rendering.
