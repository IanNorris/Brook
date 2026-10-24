# Individual Xwayland windows

Brook now has an opt-in owned rootless Xwayland path. Native controls show two
independent xeyes windows, with no enclosing X desktop. Authenticated Steam
Store and Library render in a native window, and Friends opens and moves as a
separate window. Longer Steam acceptance, broader transient policy and
shaped-window rendering remain open.

## Native geometry API

`WM_WINDOW_STATE` (`0xB08`) snapshots one calling provider's window without
mapping pixels or exposing foreign windows. Its shared 40-byte ABI is
`src/shared/wm_window_state.h`. It reports client coordinates, extent,
focus/visibility flags and creation incarnation. Zero discovers the identity;
subsequent queries can require it. Missing/foreign windows return `ENOENT`;
owned recycled slots return `ESTALE`.

`WM_PLACE_WINDOW` (`0xB09`) accepts the owned window's nonzero incarnation and
signed client coordinates, preserving size and focus. Both operations serialize
window access under the native mutex. Query output is prefaulted before that
mutex and copied after releasing it. Provider buffer sizes now preserve their
exact requested extents; interactive desktop resize minima remain separate.

The production window lifecycle host fixture passes ownership, incarnation,
placement, scaled decoration and signed-coordinate controls. The release kernel
builds; entry-state audits pass for the supported templates, together with
scheduler-start and lock/wait mutation checks. This is not universal handler
coverage. Frozen kernel SHA-256:
`f1092252021847281bf229169b2f9f2e99bb4a937c69c05c7a5c1eeb704367cd`.
`scripts/workloads/window_state_check.py --brook-guest` passes 23 native checks,
including inaccessible output, foreign ownership, placement and a 32x24 buffer.

## Owned server and private XWM

Launch with:

```sh
waylandd --xwayland /nix/profile/bin/Xwayland --xwm /nix/bin/brook-xwm --xdisplay :0
```

The launcher creates private Wayland, XWM, control and display-readiness socket
pairs. Only the exact compositor-created `wl_client` can see or bind
`xwayland-shell-v1`; UID and client-supplied names do not grant access. The shell
associates committed 64-bit serials with `WL_SURFACE_SERIAL` events in either
arrival order, scoped to a server generation. Destroying the short-lived shell
object preserves pending and committed surface association state.

`brook-xwm` accepts inherited descriptors, never ambient `DISPLAY`. XCB startup
and synchronous replies run in this worker rather than Wayland dispatch.
Composite manual redirection is required for real rootless Xwayland surfaces.
Normal X windows use Brook chrome; override-redirect windows use client pixels
without taking native keyboard focus. Root, support and InputOnly windows are
excluded from native presentation; InputOnly X map requests are still honored.
X borders are forced to zero to keep backing pixmaps and client extents aligned.

The fixed-message stream has bounded queues, framing for partial/coalesced
reads, and fail-closed generation retirement on EOF, malformed messages or queue
exhaustion. XID incarnations reject stale commands. Transient parent references
carry resolved identities. Native geometry commands carry serials so an older
X acknowledgement cannot undo a newer native drag/resize. Old-size frames wait
for matching extents, and NULL-buffer unmapping clears native incarnation state.

Focus uses input hints and `WM_TAKE_FOCUS` with an asynchronously acquired X
server timestamp. Close uses `WM_DELETE_WINDOW` when supported, otherwise the
ordinary X client-close fallback. Reparenting away from the root retires the
native association; returning creates a fresh identity. Generation retirement
removes owned X windows without disconnecting native Wayland clients.

The launcher uses a private `-displayfd` channel and checks the chosen display,
plus a bounded startup deadline. The first native attempt used `-nolock`, which
Xserver ignores for non-root callers. That attempt remains recorded; the
corrected ordinary-user launch succeeds. The new worker is deployed with the
compositor by fresh-image, image-update and private offline-staging scripts.

## Evidence and remaining acceptance

The Nix check phase exercises the actual shell and bridge adapters. It covers
both association orders, protocol errors, retained pixels, independent windows,
geometry ACK ordering, stale-size frames, NULL-buffer remap, focus/close and EOF
isolation. A mapped native xdg toplevel keeps its native identity and retained
pixels through helper EOF and server disconnect. A replacement generation can
reuse XIDs/serials while rejecting delayed messages from the retired generation.
These are host adapter controls; the corresponding native restart gate remains.
A fresh headless Xvfb control additionally exercises the real XCB
worker: InputOnly maps, border suppression, serial association, configure,
globally active client focus, cooperative close, reparent and XID reuse. The
stale-XID control also rejected a deliberately broken worker during development.
These controls do not replace native Xwayland or Steam acceptance.

In private review `steam-rootless-xwayland-n-20261024`, real xeyes windows have
exact 160x100 and 240x140 client sizes. Dragging one leaves the other stationary.
Maximize reaches a matching 1916x996 buffer; restore returns to 240x140 at the
moved position. Cropped captures show pupils responding to left/right pointer
positions. Both window buttons close their X clients. Mousepad runs as a native
Wayland client on the same compositor. The corrected compositor/worker were
hash-verified runtime overrides in this first review; repeat fresh-boot acceptance
with the corrected packaged launcher is still required.

The same review preserves the authenticated profile and runs Steam without a
loader audit or preload. Store renders at 1280x800; native maximize produces a
1916x996 Library with game artwork. Friends opens at 300x650 and a drag moves it
independently while Library stays maximized. The native Wayland editor remains
on the same compositor. These private captures establish individual-window
presentation and real application rendering; they do not yet establish all
dialogs, successful installation or long-term stability. Friends' own client
close button after movement remains an input-alignment acceptance check.

Known gaps and remaining gates:

- Finish Steam dialogs/navigation, input-alignment and a small-download
  acceptance run through the rootless path. Initial authenticated Store,
  Library, Friends and independent movement are verified.
- Exercise Xwayland retirement/restart with native Wayland clients present.
- Transient identities currently affect initial placement; later parent motion,
  minimize and stacking need broader policy/acceptance. Existing native popup
  raising also groups by provider, which can include unrelated Wayland clients.
- XShape bounding-region clipping is not implemented. Shaped xeyes buffers can
  expose undefined pixels outside the eye shapes; do not treat these captures as
  proof of clean shaped-window rendering.
- MCP semantic close rejects X windows with unknown client-origin metadata.
  Native chrome close dispatch works. Do not invent PID-based signal authority
  from untrusted X properties to bypass this limitation.
- Steam authenticated evidence, profiles and downloaded software remain private.
  The previous SDL3 host crash review disks are preserved independently.
