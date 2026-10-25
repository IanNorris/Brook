# Pointer capture foundation

Status: kernel and native SDL/Wayland relative input validated. Xwayland's
SDL motion control still fails and physical host-pointer grabbing remains
unaccepted. The checked Q3 review enables grabbing on Wayland; use an explicit
relative QEMU pointing device for a physical game review.

## Ownership and input

`WM_POINTER_CAPTURE` (`0xB0A`) takes an owned window ID, operation and token.
Query (0) returns the current matching lease or zero; acquire (1) requires a
mapped, visible, focused window in active WM mode; release (2) requires the
exact current token. Operation 3 reads an owned-window-accessible physical-click
serial, advanced only by an uncaptured, unfenced and unquarantined left press.
This allows providers to distinguish fresh interaction from queued WM events.
Tokens do not wrap into reuse. Each lease also retains
the window creation identity, so a recycled window slot cannot inherit it.

PS/2, USB and virtio relative motion now carry complete signed deltas in the
device input queue, independently of screen-coordinate clamping. Events receive
a publication sequence at enqueue. Input layout changes require module ABI 53;
all drivers must be rebuilt and staged with the kernel. Legacy consumers still
receive ordinary absolute mouse-move events through their existing wire ABI.

Captured motion, buttons and wheel events bypass desktop hit-testing. Raw
deltas are split into signed 16-bit wire chunks without wrapping or overwriting
earlier motion. The kernel cursor is hidden while captured. Focus loss,
minimization, destruction and disabling WM revoke synchronously. Device-queue
loss or a full captured window queue revokes rather than silently losing motion.
Provider queries discover revocation even when an event queue cannot notify it.

Ctrl+Alt+Esc releases capture; repeats and the matching Escape release are
consumed. Release restores the original cursor location, fences already-queued
capture-era pointer events and quarantines held buttons through physical up.
Physical-up recovery fences queued packets and retains a release tombstone for
late publication, so a dropped Up cannot wedge capture and a delayed Up cannot
reach another client. A new Down establishes a fresh pair. The same filtering
runs after switching to legacy mode.
The compositor reconciles old click latches, drag/resize state and polled
buttons on lease transitions. New input must not turn an old held button into
a desktop click. An acquisition sequence boundary likewise prevents assigning
previously published input to a new lease.

## Verified controls

`test_pointer_capture` includes the production window implementation and device
ring helpers. It checks foreign ownership, unmapped/unfocused windows, stale
tokens, slot reuse, signed motion totals and button ordering, absolute-motion
suppression, exact release position, queued input at release, held buttons,
device/window overflow, key repeats and WM disable, including queued and
late-published releases after physical-up recovery and a fresh click afterward. The existing production
window-surface lifecycle test also passes.
After rebuilding the changed input/process layouts, all 109 host suites pass.
The final release-isolation changes pass the two affected production window
suites and a focused Astra review.

The native control in `scripts/workloads/pointer_capture_check.py` creates
owned windows and waits for bounded input from the host controller. With QEMU's
PS/2 mouse selected, twenty movements of (+200, -100) produce exactly
(+4000, -2000), beyond the virtual screen edges. A left press between the two
halves arrives at accumulated (+2000, -1000). Ctrl+Alt+Esc releases while left
is held; moving over the launcher and releasing causes no popup/click-through.
Focus loss, destruction and stale-token rejection also pass. The original
guest operation returns 0 with an empty lineage and both output EOFs.

The original absolute tablet is then restored. Actual Mousepad UI launch,
typing the expected single-copy text and clicking its discard dialog work;
the window and client group exit. These controls do not establish Wayland
pointer-lock protocol behavior, SDL/Xwayland capture, physical host grabbing
or game mouse usability.

Initial native review: `build/reviews/pointer-foundation-20261025`, including
the Mousepad UI control. The final corrected kernel repeats the native pointer
control in `build/reviews/pointer-release-20261025`; input operation
`c2da6d4390a441f880c77550fbefceac`. Kernel SHA-256:
`21ae7b15a5952c32950e87f89533fc3ea1ce2767ea6cf5f4709cf0c16a2f3d80`.
The matched staging gate checks all 13 ABI-53 modules. Raw evidence remains
private under the review and bridge registry.
The original VM owner exits 0 after acknowledged final sync; all four offline
filesystem checks pass.

## Wayland and SDL

The provider advertises relative-pointer v1 and pointer-constraints v1. A lock
activates only for a pointer-focused mapped native window, inside the copied
constraint region intersected with the committed surface input region; kernel
focus/visibility ownership is checked independently. Relative motion preserves
signed deltas and emits `wl_pointer.frame` boundaries for SDL. Active locks
suppress absolute motion but retain button/wheel delivery. Lease tokens reject
queued motion from earlier locks.

Region add/subtract operations preserve holes and use wide endpoint arithmetic.
Surface input regions and constraint `set_region` changes are copied and applied
on surface commit. Region eligibility is used for constraints; ordinary pointer
routing retains its existing behavior pending full implicit-grab routing.
Confinement requests remain inactive by policy until kernel region clamping
exists. Optional cursor-position hints do not warp; release restores the kernel
acquisition anchor. Subsurface locks without their own native window stay inactive.

Seat children, relative objects and constraints retain their seat through
`wl_seat.release`. Surface and pointer destruction detach surviving protocol
objects. One-shot locks never reactivate after deactivation. Kernel revocation
inhibits capture across protocol destroy/recreate; a fresh physical click rearms
persistent locks. A failed release, including revocation between query/release,
also inhibits capture. A queued WM Down cannot fake that physical interaction.

The package's production-handler/wire test covers copied region holes, endpoint
overflow, actual surface commits, frame boundaries, button releases across holes,
stale tokens, emergency retry, query/release races and resource teardown. All
existing Wayland package checks pass as well.

`tools/pointer-probe-pkg` is a public SDL control with no game data. Native
Wayland run `11a8662c1b584f95b60298323f7b895f` receives exactly (+4000,-2000),
one press at (+2000,-1000), one release and 40 PS/2 packet motions. A second
run `4ac847f659554ccea56cd89b15ed0f39` also verifies the real protocol
locked/unlocked/locked sequence around Ctrl+Alt+Esc and a new physical click,
then repeats the exact totals. Both original jobs exit zero with empty groups
and both output EOFs. The first pre-fix run delivered all raw Wayland deltas but
SDL observed only the last piece at each button: adding pointer frame boundaries
fixes this measured integration failure.

The SDL X11 control exits one with incorrect (-845,-462) totals; it is recorded
as a failure, not Xwayland acceptance. Quake 3's actual installed shortcut starts
on Wayland with `in_nograb=0`; its kernel cursor hides in capture and it handles
normal WM close with an empty exact client group. Observed 43–45 FPS is a scene
counter, not a performance improvement or timed benchmark.

Review: `build/reviews/pointer-frame-20261025`; kernel SHA-256
`bfd7d1f49b0fd2fd65a8dc44738c3fda485878b623bb49f5c64b2eb720764d17`;
provider `/nix/store/gy0wiv4dd9alx1rjwmlrqghd77fmz4a6-waylandd-brook-0.1-brook`.
The original VM controller exits zero after checked sync and all four offline
filesystem checks pass. Game data and raw logs remain private.

For native physical review set `BROOK_POINTER_DEVICE=ps2` when using the normal
host launch script. Default `tablet` remains suited to absolute bridge input;
other values fail validation. This selects the QEMU relative device, not an
agent interface to the host desktop. QEMU frontend grabbing/release and physical
mouse-look still require live acceptance. Do not infer them from injected QMP
relative input, protocol events or hidden cursors alone.
