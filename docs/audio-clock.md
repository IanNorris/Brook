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

## Delivery loss prerequisite

The old mixer removed a batch from its ring, called the driver and ignored the
returned byte count. A short or failed write therefore discarded unaccepted
samples. The hosted `test_audio_delivery` compiles the actual `audio.cpp` with
a fake driver: the baseline loses data after a partial write and fails its
independent byte-sequence comparison.

The revised mixer retains one immutable delivery batch and its accepted-byte
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

This is a delivery prerequisite, not complete stream accounting. In particular,
existing delay/space queries still omit staged delivery bytes, stream IDs can
collide, and global stop/reset and per-stream ownership remain unfinished.

## Clocked OSS endpoint before enabling the ALSA OSS plugin

Review with Astra identified these required integration steps:

- Allocate real stream identities with serialized per-open controls and safe
  staging. Replace modulo-eight reuse and the shared additive ownership model
  with per-stream queues.
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
