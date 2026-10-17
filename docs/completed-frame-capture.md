# Completed-frame capture and recording preparation

The old bridge returned the CPU render backbuffer, or initiated GPU readback
from the debug command handler. The bridge paused QEMU later. Rendering could
advance between those operations, producing partially painted launcher/window
captures. These images were not evidence that the displayed frame was broken.

Capture API 2 requests a compositor-owned snapshot after presentation. The CPU
path copies the presented front buffer; the GPU path performs scanout readback
at the frame boundary and copies it into the snapshot. A single lease retains
those bytes until release or expiry, while the compositor continues drawing.
The host validates the lease again after RAM extraction, before publishing an
image. It restores QEMU's previous running state and releases the lease on error.

The protocol is described in the [bridge README](../tools/vm_bridge/README.md#completed-frame-capture).
Both kernel and bridge must be updated. Existing MCP processes need a reload;
source CLI calls load the new implementation immediately. No host desktop access
or plugin identity changes are required.

## Acceptance

Two isolated 8-core, 4 GiB VMs ran kernel SHA-256
`588fc9ea73cbb6e6e62ba412479bf95bc7fe9d18f6eddc7488d816aa0f5a255c`
at native 1920×1080. Settings was open at the saved 200% native desktop scale.

| Path | Session | Held frame epoch | New frame epoch | Recovery epoch |
| --- | --- | ---: | ---: | ---: |
| CPU | `092e391aad0543d9a0433c28b8fe2524` | 6156 | 6364 | 8374 |
| GPU DRAW | `1d80ac00368f44708cf0fd51e0ce3a43` | 572 | 665 | 1632 |

For each run, a held snapshot stayed byte-for-byte unchanged across a live
200% → 100% desktop scale change. A second request returned Busy, and releasing
the wrong token returned false. Releasing the original token allowed a newer
frame with a different image. An abandoned lease expired after about 30 seconds
of guest time; a subsequent capture succeeded. Both runs emitted
`CAPTURECHECK PASS` for ownership, wrong release, busy, epoch, expiry and recovery.

Both VMs completed checked filesystem/device flush shutdown. Data, Nix and boot
filesystem checks returned zero after each run. Frozen artifacts, scripts, logs,
disk clones and result JSON remain under ignored
`build/workloads/completed-frame-20261017/`.

The two-pass release build and all 26 native suites passed. All 49 bridge tests
passed, including pending/ready ordering, refusing a legacy mutable source,
busy capture, wrong-token/expired-lease rejection, releasing after read failure,
resuming QEMU and removing a partial extraction on memsave failure, and omitting
stale frame times from panic metadata. A new live panic run, allocation failure
injection, 4K and runtime display-remap acceptance remain unverified.

The source bridge passed actual VM captures. A call through this chat's older
loaded MCP process still used the old code and failed against Pending metadata;
its outstanding token was released, and an updated source capture recovered.
Reload is a compatibility requirement, not a reason to restart a live VM.

## Continuous recording remains open

This is still-capture ownership, not a video recorder. A snapshot copy, GPU
readback and QEMU RAM extraction on every frame would add substantial observer
overhead. Continuous VM recording still needs a display stream, encoding,
timestamps, audio synchronization and frame-drop accounting. The existing
[showcase plan](showcase.md#live-one-take-tour) identifies QEMU's private D-Bus
display/audio listener as the next implementation candidate. Do not begin the
final showcase or describe repeated screenshots as smooth video recording.
