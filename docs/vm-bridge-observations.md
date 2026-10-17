# Selectable VM observations — acceptance, 17 October 2026

Bridge 0.7.0 adds selectable `vm_observe` components and a boot-guarded frame
lease path. It implements another part of [the bridge specification](vm-bridge-next.md),
following the [launcher/window controls](vm-bridge-ui-acceptance.md).
Exact frame/WM revision correspondence and a lossless guest event stream remain
open; this checkpoint does not complete the specification or workload goal.

## Contract

Pass `include` with any distinct selection of `frame`, `windows`, `processes`,
`launches`, `errors`. Omitting it preserves the existing frame-only interface.
Each selected component reports its own state and host capture interval/age.
Window/task/launch metadata shares one guest exchange. Task and process-group
references, windows, launches and guarded frame leases bind the guest boot and
host generation. A final identity check marks mixed-boot results stale/partial.
No component is presented as globally atomic; frame/window correspondence is
explicitly unverified and the WM state revision is unknown.

The task snapshot copies at most 1,024 registry entries under one lock without
escaping process pointers or walking each task's entire group. Membership and
incarnations are protected; credentials, counters and scheduler state are
rolling. Ordinary app task credentials are observable without parsing `procs`.
Launch snapshots cover the retained bridge history, not every past user launch.
Bulk launch snapshots leave process liveness unknown rather than scanning all
processes for every launch; task records retain launch tags for correlation.

The errors component scans bounded serial patterns, including actual Brook user
faults, panic markers, device diagnostics and launcher failures. It is not an
exhaustive event source. Cursors retain file identity, offset, a content anchor
and oversize-line continuation state. They report resets, discarded/excluded
bytes, pending partial lines and unknown rotation loss. The first scan covers
the most recent requested window, not the oldest portion of a larger tail.
Per-line guest timestamps/boot attribution remain unknown; a log can span boots.
See the [bridge README](../tools/vm_bridge/README.md#observation-bundles-bridge-070)
for bounds, arguments, panic behavior and examples.

## Build and normal acceptance

The frozen kernel SHA-256 was
`20f62b2d0c78010b51c966dea595c15f5007961cacf4c0aef68a0393a0e0ba30`.
The unchanged `waylandd` SHA-256 was
`d7d9725ecd2e6294b1e03fdd5a16996d9cb32ff112ecf4025af37596ec2c2893`.
Tests used isolated headless QEMU, eight vCPUs, 4 GiB RAM, software composition
at 1920×1080, private cloned disks and silent host audio. No host desktop APIs
were used.

Evidence is under ignored `build/workloads/gallery-fresh-20261017/`:

- `observe-build.log`: two-pass kernel/symbol build and all 29 hosted tests.
- `observe-acceptance/observe.json`: native Terminal, GTK Mousepad and Settings
  launched through the normal launcher; corresponding task/window references
  agree and each app's real/effective UID is 1000. All components, full-resolution
  pixels, crop coordinate provenance and recursive reference-only MCP delivery
  passed. The screenshot is `observed-desktop.png`.
- `observe-final-acceptance/observe.json`: repeat with the optimized capability
  handshake, reusing the nonzero epoch already present in the capabilities reply.
  The extra boot read was redundant because all later operations are guarded.
- `observe-launch-regression/launch.json`: the full launcher recovery/negative
  acceptance also passes against the observation kernel. A removed executable
  produces actual dispatch failure `-2`, and the manifest is restored.
- `observe-acceptance/real-dispatch-error.json`: an earlier error cursor catches
  that real worker failure without replaying previous lines.
- `observe-final-check/`: ordinary checked flush/stop, exit confirmation, serial
  screening and read-only data/Nix/FAT checks, all exit zero.

Final warm-run timings with three apps present:

| Selection | Wall seconds | Guest exchanges |
| --- | ---: | ---: |
| Windows, tasks, launches, errors | 0.527 | 3 |
| Those plus full 1080p frame | 1.595 | 7 |
| Incremental errors only | 1.002 | 2 |
| 1000×700 crop scaled to 800×560 | 1.573 | 6 |

Each is one tool call. Guest exchanges include capabilities, identity checks and
frame lease operations; metadata selection itself uses one request. These are
observations from one run, not latency guarantees. Earlier incremental reads
experienced transport retries; the response now reports read-only retry count.

## Reboot and panic acceptance

`accept_observe_reboot.py --session SESSION --out build/observe-reboot --panic`
is explicitly destructive and requires an empty agent-started VM with cloned
disks. It performs a checked sync, overrides QEMU's experiment `-no-reboot`
action through `set-action`, and resets without changing the host generation.
This isolates the in-guest epoch boundary rather than relying on the host's
reset bookkeeping. The general `vm_control reset` restart-verification backlog
is not resolved by this test helper.

A second run (`97986d591a6242b5ad5c1c6a5b1e0760`) passed:

1. Capture with the first boot, then abandon another pending lease.
2. Reboot under the same host generation and initialize a different guest epoch.
3. Acquire new lease token `1`, reused from the prior boot. Old-epoch request,
   status and release all reject; the new lease stays ready and releases only
   with the new epoch.
4. Keep the old boot's cached frame geometry, sync again, and trigger the
   explicit boot-script test panic.
5. Request all components. No guest RPC is attempted. Normal metadata reports
   unavailable; serial scanning reports the panic. QEMU framebuffer inspection
   returns the visible panic/register/stack/QR screen, explicitly `unverified`
   because its geometry came from an earlier boot. This image is evidence that
   this run's fallback worked, not a guarantee that old geometry is valid.
6. Force-stop the panicked VM. Its pre-panic checked sync is retained separately;
   no post-panic flush is claimed. Read-only checks of all three disks pass.

Evidence is `observe-reboot-acceptance/reboot.json`,
`panic-frame-unverified-geometry.png`, `stop.json`, `filesystems.json` and the
retained final serial log. The expected test-panic marker was verified separately
from unexpected device/cleanup diagnostics.

## Regression coverage and remaining work

All 150 bridge tests pass (five optional encoder tests skipped). New cases cover
selection without image work, one metadata exchange, references/credentials,
partial component failure, host generation/guest boot changes, guarded lease
release, stopped/panicked availability, cached panic provenance, actual user
fault markers, log rotation/truncation/rewrite, nonzero old generation offsets,
UTF-8 partial lines, oversize continuation, event/text limits, initial recent-tail
selection, malformed cursors and nested media delivery. The existing kernel
hosted suite remains 29 tests; real-VM evidence covers the new scheduler snapshot.

Still open: frame/WM revision correlation, guest-owned event rings/typed waits,
action-attached selectable bundles, profile/audio convenience operations,
shutdown verification as an MCP operation, job cancellation and retained output
artifacts after stop. Broader real-app acceptance remains part of the active goal.

## Action bundles and saved views — bridge 0.9.0

Host-only follow-up; the kernel and Wayland provider hashes above are unchanged.
The bridge suite passes 177 tests (five optional encoder tests skipped). New tests
cover preserving action evidence across optional observation/delivery failures,
read-only recovery with different view options, preview/crop pixel provenance,
cache integrity/scoping, no-Pillow fallback, native row-copy crops, diagnostic
overlay separation and latest-source coordinate input/boot checks.

A private eight-vCPU/4-GiB 1080p VM used cloned disks under ignored
`build/workloads/action-workloads-20261017/`. VSCodium launched through `vm_launch`
with a frame/windows/tasks/errors bundle. Window presence plus bundle took 12.59s;
this was not editor-readiness timing. Native key input created and saved a 171-byte
document in `/data/home`, independently verified as UID/GID 1000:100 with SHA-256
`b9c2d352158a05d9801f5f6a923bc15fc53189a118355e14cfc6b6f2af179500`.
Minimize and restore each confirmed and returned a separate post-action bundle.

From the restored frame: tiny 320×180 (116,894 bytes), medium 960×540 (916,241
bytes), and native editor crop 820×620 (108,273 bytes). Cached preview reads took
5–10ms and a fresh crop 65ms in this run before the native row-copy optimization.
These are individual measurements, not performance guarantees. Cached view reads
passed with guest/QMP entry points replaced by functions that raise, proving no
VM exchange. Visual inspection confirmed the crop's editor text was readable.
An optional native-coordinate grid was generated and inspected from the same
saved source, retaining the clean original. Chromium then launched through the same action/bundle path alongside Codium
(window presence plus bundle: 14.79s) and native address-bar input navigated to
Google, visually confirmed with the remote page title and content. Browser
startup still emits D-Bus/inotify and cache diagnostics; rendering this page
does not resolve those compatibility gaps or complete the broader workload goal.

Both apps subsequently passed graceful close with exact client-group exit waits.
The private VM acknowledged checked filesystem/device flush and its launch
process exited zero. Serial screening found no user fault, panic, allocator
exhaustion or device-timeout marker. Read-only data/Nix/FAT checks passed after
process exit. Acceptance evidence remains in the ignored workload directory.
