# Semantic VM window controls — acceptance, 17 October 2026

Bridge 0.5.0 adds `vm_windows`, `vm_window` and `vm_ui`. It implements the
window-control portion of [the bridge specification](vm-bridge-next.md).
Catalog discovery (0.5.1) and semantic launcher activation (0.6.0) follow below;
the remaining observation/event conveniences are still pending. Reload the MCP process after updating its source;
its previously loaded tool schema does not acquire new tools automatically.

The kernel and the rebuilt `waylandd` package are both required for Wayland
connection-origin metadata. An older server leaves origins unknown and close
unsupported, rather than attributing a client window to the shared server.

## Environment and evidence

Tests used one isolated headless QEMU VM, eight vCPUs, 4 GiB RAM, software
composition at 1920×1080, and private boot/data/Nix disk images. Speaker playback
was disabled. No host desktop input/capture was used. The tested frozen kernel
SHA-256 was `f751a9e14a71b986be31945f6c41fe00c64260e138380839e1033e480939dad2`;
`waylandd` SHA-256 was
`d7d9725ecd2e6294b1e03fdd5a16996d9cb32ff112ecf4025af37596ec2c2893`.

Runtime evidence stays ignored below
`build/workloads/gallery-fresh-20261017/`:

- `ui-acceptance/actions.json`: native Terminal and GTK Mousepad focus, minimize,
  maximize while minimized, explicit maximized/normal restore, identical request
  recovery, actual Wayland buffer extent, and graceful close/group exit. Closing
  Mousepad preserved the native app and shared Wayland server.
- `ui-acceptance/minimized-terminal.json`: a terminal exec'd `busybox sleep`, was
  minimized, then closed by SIGHUP. Window disappearance and exact group exit
  completed. Terminated-owner cleanup now covers invisible/minimized windows;
  registry-backed process references and exact terminal lookup protect cleanup.
- `ui-acceptance/recovery.json`: a reused window slot rejected the old creation
  identity; takeover blocked both the host path and direct guest requests;
  release restored normal controls.
- `ui-acceptance/reboot.json`: a fresh guest boot with unchanged host session
  generation rejected old window references, marked old jobs/UI operations lost,
  and completed a new command. The experiment launcher's no-reboot policy first
  stopped QEMU; the test explicitly changed its reboot action, reset and resumed
  it. See the reset follow-up in [the backlog](backlog.md).
- `ui-job-acceptance/jobs.json`: ten command-job scenarios passed again on job
  API 2, including literal argv/environment and identity, separate streams,
  nonzero/signal exit, deduplication, delayed descendants/EOF, retention loss,
  interrupted full-pipe producers and recovery through a separate host process.
- `ui-final-build.log`: 28 hosted kernel tests passed, including concurrent
  first-wins epoch initialization and rejection of invalid/zero tokens.
- `ui-final-check/filesystems.json`: checked guest filesystem/device flush,
  QEMU exit, serial panic/I/O-error screening and read-only data/Nix/FAT checks
  completed; all three filesystem checker exit codes were zero.
- Bridge suite: 110 tests completed successfully with five optional encoder tests skipped.
  Host tests cover lost replies, read-only recovery, stale slots/boots, reboot
  between status and output, abandoned staging, and serialized two-client
  takeover/release with reversed completion pressure.

`tools/vm_bridge/accept_ui.py --session SESSION --out build/ui-acceptance`
repeats the normal workflow on an already prepared disposable VM with a clean
Mousepad document and a native Terminal. It closes both apps. It does not claim
to exercise Applications launcher activation.

## Timing and limits

Twelve final action-and-observation calls had a median of 1.54 seconds and a
range of 1.37–5.03 seconds. These are host wall times for dispatch, bounded wait
and window observation, not compositor rendering latency or sampled CPU time.
An earlier lost observation was recovered by operation ID without repeating the
mutation. Framed TCP clients now skip debug-ring replay, including fragmented
commands. The listener's idle accept sleep and per-chunk connections remain
visible overhead; no CPU-overhead reduction or broad workload speedup is claimed.

Completed WM actions are distinct from client configure acknowledgements, redraw,
window disappearance and application exit. Process liveness is sampled after the
locked window snapshot. A Wayland origin is an accepted Unix connection origin,
associated by its provider; inherited/passed sockets can outlive that origin.
It grants no authority to kill that PID. Current waits track its exact group,
not every application descendant.

Whole-VM snapshot rollback still requires a changed host generation. Legacy
coordinate input and index-based focus do not yet share the new boot guard.
Manual user-started VM acceptance, adversarial syscall registration cases and
remaining spec items are follow-ups. This checkpoint is not full-spec or
real-workload acceptance.


## Catalog discovery follow-up — bridge 0.5.1

`vm_apps` now exposes the same pinned catalog as the Applications menu. The
follow-up used the same isolated 8-vCPU/4-GiB software-rendered setup, with kernel
SHA-256 `979d1fe4ed026c94121e5b7227b1aafa778669513a03dd60481207d85b597042`.
Evidence is under the same ignored workload directory:

- `apps-acceptance/apps.json` and `catalog-events.json`: 19 original entries,
  stable IDs across refresh, an actual Mousepad menu click with a nonzero launch
  tag and registered client origin, followed by graceful window/group exit.
- Two new desktop entries with identical titles and different IDs both appeared.
  Reusing the ID too rejected the entire candidate; the serial rejection marker
  and unchanged active generation establish that the old catalog was retained.
- Removing a test executable after publication, without triggering another
  refresh, then clicking its visible entry produced `dispatch failed result=-2`.
  This exercised worker revalidation rather than just hiding the entry on rebuild.
- The original manifest/catalog was restored. Checked sync/shutdown, serial
  screening and all three read-only filesystem checks passed (`apps-final-check`).
- 28 hosted kernel tests and the 111-test bridge suite passed (five optional
  encoder tests skipped). State-machine cases retain coalescing through an early
  window, preserve later dispatch failure, reject incompatible specifications,
  and keep in-flight identity through timeout/dismissal. Exact namespace/case and
  field-separated catalog keys have hosted coverage.

`tools/vm_bridge/accept_apps.py` packages the live discovery/menu workflow;
`--negative-tests` explicitly enables the temporary manifest edits on disposable
images. The checked-in script also passed all negative cases on a second fresh boot
(`apps-script-acceptance/apps.json`); all 19 app IDs matched across both boots.
Its fixture requires an empty 1920×1080 desktop at 100% scale and at most
40 initial entries. This remains pixel-menu acceptance, not semantic activation.

Availability is validated at catalog build and rechecked by the launch worker;
it is not a fresh filesystem scan per `vm_apps` call. Duplicate legacy shortcut
and desktop entries may still represent the same package through different
launch configurations. Durable `vm_launch` dispatch/history, single-instance
outcome correlation, and the remainder of the specification are still pending.


## Semantic launcher follow-up — bridge 0.6.0

`vm_launch` now admits an exact app reference through the same compositor and
worker path as the Applications menu. The tested kernel SHA-256 was
`4f729090d7968c0368bf7c4e3330d2fde8ce010a294225fe641ca669d92f70db`,
with unchanged `waylandd`, the same isolated 8-vCPU/4-GiB software-rendered setup,
and host speaker output disabled.

Evidence under `build/workloads/gallery-fresh-20261017/`:

- `launch-build.log`: two-pass kernel/symbol build and all 29 hosted kernel tests.
- `launch-acceptance/launch.json`: native Terminal and Wayland Mousepad launch,
  exact-tag correlation, repeated request-ID recovery, second app request,
  graceful close of each actual window, stale catalog rejection and takeover.
- `launch-complete-acceptance/launch.json`: the complete checked-in
  `accept_launch.py --negative-tests` workflow. It additionally removes a probe
  executable after catalog publication and retains actual dispatch failure
  `-2`; the original manifest/catalog is restored in `finally`.
- The second Mousepad request initially observed an existing app-ID match;
  its later status strengthened this to the new window's exact launch tag.
  This is correlation evidence, not proof of a single-instance activation ACK.
- `semantic-launches.png` in those acceptance directories retains the raw
  1920×1080 completed-frame screenshot. No preview envelope was serialized.
- `launch-final-check/`: checked sync/stop, serial screening, all three
  read-only filesystem checks, and labelled cached launch recovery after stop.

The bridge regression suite passed 123 tests (five optional encoder tests
skipped). New host tests cover lost mutation/status replies without replay,
request-ID binding across operation kinds, stale catalog/boot, takeover,
unsupported guest capabilities, wait cancellation without launch cancellation,
window disappearance/incarnation reuse, and dispatch failure despite an early
window. Admission/dispatch waits remain valid when a windows query fails;
stopped-session recovery retains known dispatch facts while current windows
remain unavailable. Hosted history tests cover adoption of prior UI window
observations and timestamps, weak-to-exact correlation, timeout/late completion,
capacity/tombstones and cancellation.

History and correlation are bounded as documented in the bridge README. Human
takeover cancels queued agent-only work; a frontend request already selected for
dispatch, a started worker or a launch shared with the user may continue. Live
queue saturation, this in-progress takeover race and a true application's
single-instance acknowledgement are not claimed by this acceptance. The first
negative test attempt used an incorrect Escape-to-dismiss fixture; it was fixed
to click outside the menu and the full workflow passed afterwards.
