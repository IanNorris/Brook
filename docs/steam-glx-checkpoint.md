# Steam GLX checkpoint — 23 October 2026

The subsequent [shared-memory checkpoint](steam-shared-memory-checkpoint.md)
addresses the controller IPC and `/dev/shm` boundary described below.

Hardware GLX now creates a real X11 window, binds an OpenGL 4.6 context on
virgl / Intel UHD Graphics 630, clears it red and reads back exactly
`[255, 0, 0, 255]`, with no GL or X errors. This is a rendering compatibility
check, not a presentation/performance benchmark or Steam login acceptance.

## Fixes

Xwayland's DRI3 connection passes a DRM render-node descriptor using SCM_RIGHTS.
Brook supported PRIME buffer descriptors but rejected the render node itself
with EBADF. Queued DevDri descriptions now retain the ordinary dup reference;
discarding an undelivered right uses ordinary final-close DRM retirement.
Receiving transfers that reference into the published descriptor. Module ABI
remains 47. The 65-case native DRM probe includes original-close-before-receive,
dup, fork, queued discard, ancillary truncation and failed-send rollback. Tiny
GEM resources verify the received description retains its resource table.

After descriptor passing worked, xshmfence stalled in glXMakeCurrent. A bounded
trace showed the client waiting on a four-byte memfd mapped at one address,
while Xwayland woke the same object at a different address and matched zero
waiters. Brook's old process/virtual-address futex key could not match them.

Non-private shared-memfd futexes now use a tagged monotonic object identity and
byte offset. Private addresses retain their thread-group/virtual-address key.
The identity allocator saturates instead of reusing old keys. A mapping
transaction spans key derivation, access/coverage and resident-leaf validation,
value comparison and queue insertion. Prefaulting happens before those locks;
a concurrently replaced lazy mapping returns EAGAIN instead of faulting under
the locks. Locks are released before blocking. Cancellation/rearming retain
only the chosen scalar key and existing process reference.

This implements shared memfd and anonymous-shared backing, not general shared
file futexes. Shared WAKE_OP and PI operations are explicitly rejected; private
WAKE_OP's existing behavior is not certified as a new atomic implementation.

## Reusable guest diagnostics

Run `scripts/workloads/xwayland_check.py` inside Brook for a rootful diagnostic
server on :0, followed by `scripts/workloads/glx_check.py` as its client.
Glamor GL is the default; `--glamor off` preserves the old `-shm` test mode.
Rootful startup, `-ac` and `-nolock` are guest-only diagnostics, not desktop
launch policy. No host display is selected by the server helper.

The Steam helper also includes the installed Mesa GLX vendor's canonical
Nix-store directory in its library path. Steam's bundled libraries retain
priority. Earlier tests disabled Glamor or omitted libGLX_mesa, so merely
advertising GLX 1.4 was insufficient.

## Acceptance and remaining work

The native shared-futex probe passes 19 cases: distinct-address descriptor
aliases, offsets, independent objects, bitsets, private isolation, anonymous
shared mappings, interruption, cancellation and shared/private remap stress.
The stress performed 71,301 replacements. It exercises races without claiming
a deterministic instruction-level prefault/admission interleaving. Existing
FUTEXSTRESS also passes: 1,568,639 waits, zero unexpected errors, with 16 waiters,
eight page-table churners and two wakers. The 101 host tests pass sequentially.
Entry-state and scheduler mutation audits pass on the tested ELF. Astra reviewed
the admission/lifetime invariant and found no blocking lock-order defect.
The final VM stopped cleanly after all tracked jobs drained and its heap check
passed; boot, data, Nix and temporary disks each passed offline fsck with exit 0.

Steam's retry no longer reports its GLX visual-selection failure. It still
exits with controller IPC assertions and container /dev/shm failures; no usable
login window, QR code or authentication is claimed. Only fixed diagnostic
counts were collected for this retry after full-log export was rejected by
automatic approval review. Namespace-local /dev/shm is a separate next step.
The staged client uses prior ELF-interpreter patches and skips bootstrap;
updater acceptance is not claimed. An earlier pbuffer readback failed and has
not been re-certified; the successful check uses a real X11 window.

Ignored evidence: `build/workloads/steam-glx-20261023/` (baseline),
`build/workloads/steam-glx-drm-20261023/` (descriptor fix and fence trace), and
`build/workloads/shared-futex-20261023/` (final acceptance). Final tested ELF:
`17a02ba4b150dc78c6c0a9dc908c63f5c83693110c8dc3036e06d33213e953ae`.
The source has a subsequent comment-only correction. Earlier compile/controller
failures, two host timeouts during a concurrent build (both separately passed),
and alarm-terminated GLX attempts remain recorded as failures, not acceptance.
