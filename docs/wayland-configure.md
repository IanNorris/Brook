# Wayland configure serials and mapped updates

The server uses one serial allocator for initial toplevel/popup configure,
popup reposition, activation, resize and input events. A surface records the
serial it emitted; it does not generate serials independently. Previously the
initial configure incremented a surface-local zero counter to 1, while the
first activation could also take 1 from a separate global counter. Chromium
could acknowledge the initial event but leave the activation unacknowledged.

`xdg_acked` gates the initial mapping handshake. A fresh xdg-surface role resets
it. Activation and resize do not reset it: a mapped client can continue
submitting its existing state while another configure is pending. Previously
those events closed the gate, so SHM commits copied and released client buffers
without publishing their retained pixels to the window. The same gate also
affected dmabuf presentation. This repair does not implement a configure-history
validator, full unmap/remap semantics, or other missing shell protocol features.

## Regression coverage

`tools/waylandd-pkg/configure-surface-test.c` connects real libwayland client and
server endpoints and creates an actual SHM pool and buffer through wire
requests. It invokes the production surface/xdg handlers directly and parses
outgoing configure events. It verifies:

- The initial handshake still gates publication before its acknowledgement.
- Initial mapping publishes the real SHM pixels into a mock window buffer.
- First activation emits a different serial from the initial configure.
- Later SHM commits publish while activation or resize acknowledgement is pending.
- Initial popup and reposition events also use distinct shared serials.

It is a handler/SHM integration test, not a complete Chromium protocol test.
The package also runs its existing text-input, frame-callback, retained-surface
and dmabuf-import tests.

Two negative controls were run before the complete repair:
`build/configure-surface-before.log` fails the distinct-serial assertion with
the original production code; `build/configure-surface-serial-only.log` then
fails the post-activation pixel-publication assertion with only serial
allocation repaired. `build/configure-surface-fixed.log` passes all five tests.

## Guest evidence

The original isolated 8-CPU/4-GiB hardware-GL run is recorded under
`build/workloads/mixed-render-20261019/4g`, session
`3ec17c76f80e4623b5597244d58c0f2d`. Chromium was the first application. The local
media fixture received readiness reports and the kernel window title changed,
but the displayed browser still showed New Tab. A paused read of the verified
832×642 window buffer showed the same stale pixels as the completed GPU frame,
locating the problem upstream of final GPU composition. Memory was plentiful;
the captured profile was mostly idle. This was not an OOM reproduction.

The serial log showed an initial configure acknowledgement of 1 followed by
focus, with no further acknowledgement until a recorded maximize probe produced
serial 139. That probe recovered the fixture's displayed content. The exact
serial collision in that live run remains an inference: the server's retained
image and gate were not captured before maximizing. The two host negative
controls demonstrate the corresponding defects independently.

The failed run ended with its Chromium launch lineage drained, audio streams
empty, checked guest sync/shutdown, owned launcher exit 0, all three offline
filesystem checks returning 0 and all nine document fixtures unchanged. This
endpoint does not count as successful mixed-browser/Quake acceptance; Quake
was not launched in that run.

The repaired package was then tested on fresh private disks, with Chromium
again the first application: session `43758751355440cc913157e830092e32`, evidence
under `build/workloads/mixed-render-20261019/4g-configure`. The serial log records
acknowledgements 1 and 2; initial navigation visibly rendered the fixture
without reload, maximize or resize. Muted local-video playback was captured,
and guest audio submission/completion counters advanced. A 57.106-second
source-timestamped batch recorded 1,319 decoded frames, seven dropped frames
and 22.90 video-frame callbacks per second. This establishes local playback,
not audible-quality or public-site acceptance.

The same guest launched Quake II GL3 at 1280×720 alongside Chromium. It later
hit the existing 4,095-resource allocation ceiling (`allocations=4096`), then
faulted in Mesa after repeated resource-creation failures. This is a separate
failure from the configure repair and from physical-memory exhaustion. The
attempted visible/minimized comparison is invalid: later profiles contain
Chromium alone after Quake exited. Keep the profiles as diagnostic evidence,
not a performance improvement claim. This run also completed checked shutdown,
drained both launch lineages, returned owned launcher exit 0, passed all three
offline filesystem checks and preserved the nine document fixtures.
