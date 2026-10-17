# Audio device clock and delivery accounting

Firefox's ALSA-selected audio IPC path initializes but repeatedly loses frames.
The current ALSA configuration writes raw PCM to `/dev/dsp` through a file PCM
with a null slave. This provides audio bytes without the device's playback clock.

## Measured delay mismatch

`scripts/workloads/alsa_clock.py` opens the guest's configured output, writes a
quiet stereo tone and compares `snd_pcm_delay` with `SNDCTL_DSP_GETODELAY` on
the same open OSS descriptor. It does not open an unrelated second DSP stream.
Run it only in the isolated VM, with the silent QEMU backend:

```sh
/nix/profile/bin/python3 /boot/ACLK.PY /nix/store/3innqpmxwvmr2vc8h51g47aqdl6zj2b4-alsa-lib-1.2.15.3/lib/libasound.so.2
```

The first small write stayed buffered in ALSA and observed no OSS queue. The
continuous version then wrote 32,768 stereo frames. Across twelve observations,
ALSA reported zero delayed frames while the same descriptor reported between
5,680 and 41,552 queued bytes. At 44.1 kHz stereo S16_LE these correspond to
about 32–236 ms of audio. OSS's current query is itself approximate, but the
ALSA route does not reflect even that nonzero queue.

Evidence: `build/workloads/audio-clock-20261017/continuous/result.json` and
its retained request/source hashes. The VM used eight CPUs, 4 GiB RAM, frozen
kernel `d3712dd8db582652315e6634e7aeab434c17d43b250e44fc19acdd5d5aafa928`,
and clean clones of the earlier device-accepted baseline. Both probes exited
zero, the desktop returned to ten tasks, checked shutdown completed and all
three offline filesystem checks passed. No unexpected device/serial error was
found. Probe success means observations were collected, not that timing is correct.

## Delivery loss prerequisite (17 October)

The old mixer removed a batch from its ring, called the driver and ignored the
returned byte count. A short or failed write therefore discarded unaccepted
samples. The hosted `test_audio_delivery` compiles the actual `audio.cpp` with
a fake driver: the baseline loses data after a partial write and fails its
independent byte-sequence comparison.

At the 17 October checkpoint, the revised mixer retains one immutable delivery batch and its accepted-byte
cursor until the driver consumes it. Zero and negative returns preserve the
suffix. An atomic ownership flag prevents another flush from overwriting the
batch while the driver is sleeping; the mixer spinlock is released before
the driver call. Writers can continue filling the separate mix ring.
Global stop invalidates the retained suffix at both entry and completion;
the sole flush owner checks this epoch before submitting and after the callback.
This also covers a partial batch created while stop itself is sleeping. An
already-started hardware write cannot be retracted by this mechanism.

Hosted regression covers short writes (including arbitrary byte boundaries),
zero/error returns, a second flusher and writer while a callback is blocked,
multiple ring-index wraps, saturated mixing, partial→stop→retry, a new batch
formed while stop is sleeping, and a callback straddling stop. Production HDA
returns frame-aligned writes; the byte-granular case checks the generic driver
return contract. The production flush caller is the non-cancellable mixer
worker; cancelling a different flush owner is not covered. Permanent driver
errors retain bounded pending storage rather than silently losing the batch.

The two-pass release build, supported compiled-entry gate and all 32 hosted
suites pass (`build/audio-delivery-build-20261017.log`). The isolated production
VM boots kernel
`f2e4fe9d0b677fd39d2fa2b00226cba0c20d8b803f5064e851a645f8ae501a3d`.
The cold clock probe completes successfully; its commit response initially times
out, and the same operation is observed to completion without relaunching.
An intentional warm repeat uses a ten-second lead-in to align capture and then
120 write/sample rounds. The requested ten-second QEMU capture contains 37,121
active PCM frames (0.842 seconds at 44.1 kHz), with RMS 0.0432 for the quiet tone.
Inactive lead-in time does not become WAV samples in this capture path. This
is a playback smoke check, not a ten-second continuous-playback timing test.
The process exits zero with no remaining tasks; a subsequent two-second capture
has zero PCM frames, and the desktop returns to ten tasks. Checked shutdown,
the serial/device gate and all three offline filesystem checks pass.
Evidence is under `build/workloads/audio-delivery-fixed-20261017/`.

That checkpoint fixes delivery loss, not complete stream accounting. Its stream
identities, shared ring and global close/reset are superseded by the ownership
work below; hardware completion and a trustworthy device clock remain open.

## Independently owned software queues (18 October)

Each open DSP description now owns a generation-tagged stream and a bounded
16,384-frame FIFO. Exhaustion returns EBUSY instead of cycling through active
slots. Dup/fork share the description; only its final reference releases the
stream. Reset, pause and close affect that stream's unaccepted FIFO data, without
stopping the shared hardware device. New writes and controls are serialized by
a per-description mutex. User data is safely copied into bounded kernel staging
before taking the mixer spinlock; sendfile supplies an explicitly trusted kernel
source. Blocking capacity waits release the mutex and are interruptible.

A flush snapshots each contributor's identity, reset epoch, FIFO front and
contributed frame count. Only driver-accepted frames are consumed. Whole-frame
retries remix live contributors, including after clipping, so resetting or
closing one stream does not leave its unaccepted samples in another's mix.
A partially accepted encoded frame retains an immutable tail; finishing it
cannot consume data from a replacement stream or reset epoch. Pause preserves
the identity needed to consume an already accepted prefix exactly once.

GETOSPACE now reports software FIFO space in the negotiated format instead of
a stale global hardware-capacity constant. Rate negotiation reports a supported
4,000–192,000 Hz range. A 64 KiB fragment no longer truncates to zero. Requested
fragment count remains unsupported; this is not complete OSS geometry support.
The existing nearest-neighbour resampler remains stateless across writes.

Hosted production-mixer tests cover partial/error writes, late append,
close/reuse while a callback is blocked, reset after saturated mixing, pause and
reset during partial-frame completion, unequal stream lengths, slot exhaustion,
stale identities, full queues and ring-index wraps. The two-pass release build,
supported entry-state audit and all 32 hosted suites pass in
`build/audio-owned-queues-build-v2-20261018.log`. The bootstrap acceptance gate's
expected suite count is updated to 32; its five observer tests pass. This does
not itself rerun either guest bootstrap path.

The isolated eight-CPU, 4 GiB production VM boots frozen kernel
`7adc4527c6f230078a1789eff98048f3055cc14a3fd14370559632fb40829ffa`.
`scripts/workloads/dsp_ownership.py` passes real-syscall checks for independent
reset/close, dup and fork final-reference lifetime, exhaustion/reuse, nonblocking
backpressure, extreme-rate negotiation, 64 KiB fragments, and a paused/full
writer released by another thread's reset. The job exits zero with no live tasks.

An initial candidate passed ownership but a 200-round ALSA clock probe aborted
at `snd_pcm_file_drop` with buffered bytes remaining. The
[upstream file PCM implementation](https://github.com/alsa-project/alsa-lib/blob/master/src/pcm/pcm_file.c)
stops its flush after a short write and asserts that drop emptied the buffer.
Restoring blocking full-chunk writes fixes the same reproducer: all 200 rounds
and cleanup now exit zero. Nonblocking writes still report partial acceptance.
The requested three-second QEMU capture contains 132,526 active PCM frames
(3.005 seconds), RMS 0.04322 and no entirely silent analysis bins. This is a tone
smoke test, not proof of glitch-free timing. A subsequent two-second idle capture
contains zero PCM frames. The desktop returns to ten tasks; checked shutdown,
launcher exit zero, serial/device gate and all three offline filesystem checks
pass. Final evidence is under
`build/workloads/audio-owned-queues-v2-20261018/`; the failed initial reproducer
is retained under `build/workloads/audio-owned-queues-20261018/`.

Hardware-accepted mixed samples cannot yet be independently retracted or charged
to contributing streams. GETODELAY still includes the global hardware queue;
GETOPTR, accurate completion, readiness and drain remain unfinished. Firefox
normal-launcher audio is not accepted, and ALSA/launcher defaults are unchanged.


## Clocked OSS endpoint before enabling the ALSA OSS plugin

Review with Astra identified these required integration steps:

- Preserve the independently owned FIFO and serialized safe-staging foundation
  above while adding hardware accounting; do not reintroduce shared ownership.
- Account for accepted, hardware-committed and completed frames, including a
  pending delivery batch and partial/error driver writes. Track contributing
  stream generations through hardware completion.
- Expose trustworthy HDA progress and discontinuities. Current counters reset
  on recovery, cyclic LPIB polling can miss a complete wrap, and short sounds
  below the current 8,192-byte start threshold need explicit drain/start handling.
- Derive GETOPTR, GETODELAY, GETOSPACE, readiness and interruptible SYNC from
  consistent accounting. Implement stream-local trigger/reset/close and exact
  supported fragment geometry. Do not call sleeping HDA operations under mixer
  or descriptor-scan spinlocks.
- Keep the initial ALSA plug slave at 44.1 kHz, stereo, S16_LE. Exercise short
  sounds, pointer wraps, two streams, close/reset, dup/fork, exhaustion/reuse,
  backpressure, interrupted drain and delayed service before Firefox acceptance.

The [standard ALSA OSS plugin](https://raw.githubusercontent.com/alsa-project/alsa-plugins/v1.2.12/oss/pcm_oss.c)
uses GETOPTR's ring pointer and negotiated ALSA geometry directly. Adding a
fictional pointer or selecting this plugin before the endpoint contract works
would not solve the clock problem. Launcher and ALSA defaults remain unchanged.
