# Steam API and software presentation isolation

This follows the [read-only netlink checkpoint](netlink-route-checkpoint.md).
All game files, API libraries, account data and raw captures remain private.

## Real Steam API controls

The owned [API probes](../tools/steam-api-probe-pkg/README.md) isolate calls
without linking or shipping a game or Steam SDK implementation.

The Win64 probe temporarily occupied Goose's configured executable path in a
disposable review and was launched normally by Steam, using the game's original
API DLL and Proton. Init, Friends017, two nonempty persona results, callbacks
and Shutdown completed in 4.754 guest-clock seconds. Steam's tracked root exited
zero; the diagnostic log contained no persona IPC assertion. The exact original
game executable, mode and timestamp were restored before the real-game retry.

The native SysV SteamClient020 comparison in Runtime 4 completed the pipe,
global-user, Friends017, two persona calls and user/pipe release in 3.054 seconds,
with actual child exit zero. This manually invoked native context is not the
identical Steam-dispatched Windows frontend. Neither probe logs persona strings.

Both used the original SDK discovery link. These passing controls do not prove
all Steam IPC works, but do rule out treating the game's assertion as sufficient
evidence of a generic persona-call failure. The original game still displayed
a white window and emitted an internal persona assertion; its internal code is
not decoded as errno or another Steam enum.

## Native thread evidence

The game main thread waits through a Wine server reply pipe. Reading Wine's
saved syscall frame alone is insufficient to identify the native worker call:
native pthreads can inherit the parent's GS/TEB and its older Windows frame.

The actual native ELF loader map was recovered from guest memory: initial
auxiliary vectors, DT_DEBUG and the loader's linked list, using that process's
page tables. Bounded native stack captures then resolved against the exact
mapped Mesa library. Candidate return addresses are distinguished from an
unwound call chain.

One worker is in `x11_wait_for_present`, through
`u_cnd_monotonic_timedwait`; the presentation and event queue workers are idle.
This matches a concrete Mesa 26.0.6 software X11 path gap: PutImage returns an
image for reuse without advancing the present ID, while the ordinary ID update
depends on X Present events that PutImage does not request.

The extended owned Vulkan WSI probe confirms correct red pixels through
XGetImage, followed by failure waiting for present ID 1. Without present-wait,
the same build passes all four red/green/blue/white readbacks. Rendering remains
on hardware Venus; `sw,noshm` selects software X11 presentation only.

The finite wait returns device-lost instead of timeout. Mesa's wait helper
converts errno ETIMEDOUT to C11 `thrd_timedout`, but this WSI caller compares
the returned status against ETIMEDOUT. This is a separate timeout-reporting
error, not evidence that the GPU was lost.

## Candidate and acceptance boundary

The isolated [Mesa candidate](../tools/mesa-venus-brook-pkg/default.nix) adds
checked PutImage processing before monotone present-ID completion, checks for
connection failure and updates before image reuse. It also corrects timeout
status comparison. Its per-request checks favor correctness and can be costly
for row-by-row damaged regions. It is not a default desktop Mesa replacement.

The pinned candidate builds and passes the native controls in a fresh review
with the same kernel and host driver. All eight present IDs complete, each
zero-time completed-ID wait succeeds, and a destroyed window returns
VK_ERROR_SURFACE_LOST_KHR. Old and candidate drivers both retain the ordinary
four-color readback pass. No Goose gameplay or audio acceptance is claimed
from the native diagnostic result.
The Vulkan probe's `--present-wait` mode checks eight frames and zero-time waits
for already completed IDs; the ordinary four-color mode remains available.

The candidate library SHA-256 is
`1e6b4bc864f5bc3b16071917bd413e88e0dce888751445f399be26d4d809bf71`;
the final owned probe is
`63fce816fd6c07efd39fc36ced4d412e05d19f4f0b8aae1e37476d25edb41a45`.
The first narrowed Mesa build compiled but failed packaging because the pin
declares a converter output even when its driver is excluded. The isolated
derivation now creates that unused output directory explicitly. A copied
review also initially lacked its pinned boot artifacts and failed before QEMU
started; those artifacts were copied before the successful fresh boot.

## Real-game retry with the candidate

The original installed game now renders its title menu. Space confirms menu
choices, reaching the single-player and save-selection screens. Source X11
pixels also contain the rendered menu, independently of the compositor frame.
Reading the real game's native loader map confirms the candidate library is
mapped; an older passive environment receipt alone would have been misleading.
No API DLL, game executable or original Proton settings were changed for this
retry. The internal persona assertion still occurs despite the working menus,
so it is not sufficient evidence of the original presentation blocker.

Scene loading remains unaccepted. An attempt to choose the visibly empty second
slot did not capture the changed selection before confirmation; do not treat it
as a verified new-slot test. Unity later logs achievement restoration from a
save. Original save contents and timestamp nevertheless remain unchanged.
The loading screen persists, while native condition-wait sequence values advance
between captures. This is no longer the first-present completion stall.

A five-second profile contains 2,437 samples and 125,231 context switches but
reports 152,299 dropped events. The export is explicitly lossy and does not
justify timing percentages or a loading-throughput conclusion. Bounded native
and Windows wait stacks are retained privately for the next discriminator.

WM_DELETE removes the game window and Steam removes it from its running list.
The tracked launcher returns zero; individual Wine process statuses are unknown
(`-1` in Steam's tracking log), not independently verified direct exits. No
gameplay, audio or save/load compatibility claim is made.

The subsequent normal Steam shutdown returns zero with zero live descendants
and both output streams at EOF. Checked VM stop confirms filesystem/device
flush; the original launcher exits zero. This review's serial check and all
four offline filesystem checks pass. Original Stardew, Goose and retained
Graveyard save checks retain the content/timestamp guarantees listed below.

## Review closure

Diagnostic Proton settings were restored byte-for-byte. Stalled game cleanup
needed the stock prefix-specific wineserver stop, and serial logs recorded
exit-group cleanup timeouts. These prevent claiming a clean game-exit pass.
Steam subsequently exited zero. VM stop confirmed filesystem/device flush and
the original launcher exited zero. Independently run offline checks of boot,
data, Nix and temporary disks all returned zero; the serial rejection remains
preserved separately.

All 33 original Stardew file contents and non-cloud-marker timestamps are
unchanged. Goose's original save contents/timestamp and both original Graveyard
save file contents are unchanged. No original save was intentionally loaded.
