# Pointer capture foundation

Status: kernel mechanism validated; Wayland/SDL/Xwayland integration and
physical host-pointer grabbing remain open. No pointer-constraints global is
advertised yet. Q3 retains its no-grab setting until the full path is tested.

## Ownership and input

`WM_POINTER_CAPTURE` (`0xB0A`) takes an owned window ID, operation and token.
Query (0) returns the current matching lease or zero; acquire (1) requires a
mapped, visible, focused window in active WM mode; release (2) requires the
exact current token. Tokens do not wrap into reuse. Each lease also retains
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
