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

At that checkpoint, hardware-accepted mixed samples could not be independently
charged to contributing streams and GETODELAY included the global hardware queue.
The native accounting below supersedes those limitations on supported clocks.
Independent retraction of already mixed hardware data remains unfinished.
The native OSS pointer/drain checkpoint below supersedes the earlier missing
GETOPTR/SYNC status. Firefox normal-launcher audio is not accepted, and
ALSA/launcher defaults are unchanged.

## Native DMA completion and restart acceptance (18 October)

An optional driver contract now returns bounded native-format submission
receipts and an epoch-tagged DMA completion watermark. A bounded mixer ledger
retains each contributing stream's identity and reset generation until hardware
completion. Hardware acceptance alone no longer frees application capacity;
GETODELAY charges only that description's queued and in-flight contribution.
Reset/close/reuse cannot credit a replacement stream with earlier completion.
Classic poll uses cached per-stream capacity without sleeping driver calls
under descriptor locks. Format changes with outstanding data return EBUSY.

HDA progress uses LPIB movement, not elapsed time to manufacture completed
bytes. Clock loss, rollback, device errors, unsafe publication and an ambiguous
sampling gap preserve the last proven watermark and report discontinuity/EPIPE.
The elapsed-time guard uses a coherent, ordered KVM pvclock snapshot only when
both stable-clock feature and runtime flags are present. Unsupported clocks
retain legacy delivery. Half a nominal ring duration is a conservative sampling
policy with prefetch margin, not a certified maximum device transfer rate; this
does not fix or certify Brook's separate ordinary clock reader.

Finite sounds start after a bounded priming interval even below the streaming
threshold. Physical DMA continues across a zero-filled 8,192-byte trailer before
stopping, allowing QEMU's codec FIFO to emit the tail. Idle intervals have no
application owner and cannot advance stream completion. DMA completion is not a
guarantee that a host backend or speaker has emitted those samples.

The exact-sample capture caught a second restart fault: QEMU preserves old LPIB
through SRST but resets the prepared BDL position when RUN rises. Reading the
pre-RUN value as the baseline invented a wrap, prematurely completing every
other sound. Native and legacy starts now baseline the fresh ring at zero,
before RUN. The production-driver MMIO harness models these distinct reset
effects and verifies consecutive starts with nonzero stale positions.

`dsp_short_tail.py` emits 30 stereo sounds of 1, 16, 128, 441 and 1,024 frames,
each with a distinct final negative marker. A host/guest barrier starts recording
before releasing the source; all source execution must finish within capture.
The failed candidate delivered exactly 4,830 of 9,660 nonzero frames. The corrected
candidate delivers all 9,660 in exact order, with equal stereo channels, in each
of persistent-open, close/reopen and stream-reset modes. Each job exits zero
without remaining tasks. This tests complete short-sound delivery, not continuous
latency or jitter; QEMU capture omits inactive intervals.

The same candidate passes real-syscall ownership and independent-completion
probes, including paused-stream isolation, short tails, format-change rejection,
full-queue poll and reset. All 200 ALSA write/sample rounds and cleanup exit zero.
A three-second sustained capture contains 132,502 PCM frames (3.00458 seconds),
RMS 0.043218 and no entirely silent analysis bins. The subsequent two-second idle
capture has zero frames. These are playback smoke checks, not glitch-free timing
or Firefox acceptance. The ALSA file-over-null route still reports zero delay
while its actual DSP descriptor has queued data, so its clock mismatch remains.

Evidence: `build/workloads/hda-ledger-idle-v3-20261018/`, frozen kernel
`d00f816bd224a614adc67dadd9a4f01fbaa528133fb4c74cf46d00bf50816984`,
eight CPUs, 4 GiB RAM, silent host backend. The alternating-loss baseline is
retained in `hda-ledger-idle-v2-20261018/gated2-tail-persistent/`. The corrected
VM returns to ten tasks; checked shutdown, launcher exit zero, serial/device
checks and all three offline filesystem checks pass. Two-pass release build,
supported compiled-entry audit and all 34 hosted suites pass in
`build/hda-restart-build-20261018.log`. Host stress tests ran before VM launch
to avoid contaminating playback measurements.

The expanded HDA module also exposed the loader's fixed 64-section metadata
array. Validated ELF section metadata now uses a bounded heap allocation instead
of a compiler-dependent section limit or large stack array. Table bounds reject
overflow/truncation, allocation failures fail cleanly, and the invalid-symbol-table
return frees its allocated pages. Hosted tests cover header boundaries and metadata
lifetime; the VM verifies actual module load. This is not a full ELF-loader audit:
extended section numbering, the module registry limit and image-size policy remain
separate work. The optional driver interface requires module ABI 16 and a complete
matching module rebuild.

## Native OSS pointers, geometry and captured-target drain (18 October)

GETOPTR now exposes completed application bytes and the position modulo the
negotiated stream capacity at 44.1 kHz. Mono/U8 and stereo/S16 have exact frame
mapping at that rate. Other rates and unclocked drivers return EOPNOTSUPP rather
than derive an inaccurate pointer through the write-local resampler. Byte count
uses positive 31-bit wrapping. As in Linux's non-mmap OSS path, `blocks` reports
queued whole fragments; this is not the mmap query-delta convention. Brook does
not advertise OSS mmap. Byte-counter overflow boundaries are not runtime-tested;
the real guest regression covers repeated ring-pointer wraps.

SETFRAGMENT now enforces a supported native-rate request exactly, including its
nonzero fragment count. Unsupported sizes/counts return EINVAL, and non-native
rates return EOPNOTSUPP. A zero count selects the full available capacity. The
actual mixer limit, write chunk size, GETOSPACE and GETOPTR modulus all use the
same negotiated capacity. RESET preserves geometry; a newly opened description
gets default capacity. Geometry changes require RESET after any submitted data.
This replaces the earlier silent acceptance of oversized fragment counts.

SYNC starts a paused finite stream, captures its submitted-frame target and
waits interruptibly for that target's proven completion. It releases the open
description's mutex while waiting, retaining the fd pin. Later writes do not
extend its target. Reset, pause or reconfiguration cancels the old wait with
ECANCELED; discontinuity returns EPIPE. A two-second no-progress guard returns
EIO without crediting samples. A successful drain retains cumulative counters
until explicit RESET; this differs from Linux's prepare-needed-after-SYNC state
and avoids discarding later concurrent writes. The guard's stalled-device timeout
is implemented but has not been directly exercised by the guest acceptance run.

DSP epoll now uses cached per-stream readiness and reports discontinuities as
EPOLLERR. Poll/epoll waits that watch DSP use a five-millisecond safety rescan;
this is bounded polling, not a new IRQ notification mechanism. Readiness currently
means room for a frame, rather than a complete negotiated fragment. Fragment
threshold readiness remains an integration follow-up. Driver queries, including
the approximate legacy delay fallback, occur outside the per-open mutex and
descriptor scans.

`dsp_endpoint.py` passes real guest checks for exact 4x1,024-byte geometry,
oversize rejection, nonblocking capacity enforcement, EFAULT on an invalid output
pointer, repeated ring wraps, shared dup state, independent streams, U8 mono,
epoll full/drained readiness, and unsupported non-native pointers. It also passes
drain cancellation on reset/pause, concurrent later writes not extending the
captured target, unmasked-signal EINTR and a masked signal remaining pending until
after successful drain. The initial timer-driven probe failed because setitimer
is unsupported. The corrected probe uses pthread-directed signals and exits zero
with no remaining tasks. These checks do not certify all OSS ioctls or mmap.

Evidence: `build/workloads/oss-endpoint-20261018/endpoint-v2/` retains the corrected
probe's complete source in its request and the authoritative completion. The
frozen kernel is
`ae3b46ff2ba527cbe9d91fee3fadecc7ee1ae0742439453d4f2548b8a9741b99`.
All 34 hosted suites and supported compiled-entry checks pass in
`build/oss-endpoint-build-20261018.log`, including production mixer tests that
keep hardware-accepted data charged against a smaller negotiated capacity.
The existing ownership and independent-completion guest probes pass again. An
additional persistent-open capture delivers all 9,660 expected nonzero frames
in exact stereo order, including every final marker. The source finishes within
the capture interval.

The 200-round existing ALSA file-over-null probe and cleanup also exit zero.
Its three-second sustained capture contains 132,592 PCM frames (3.00662 seconds),
RMS 0.043217 and no silent analysis bins; the two-second idle capture has no
frames. The desktop returns to ten tasks. Checked shutdown, launcher exit zero,
serial/device checks and all three offline filesystem checks pass. This preserves
the earlier playback smoke result without accepting the file PCM's clock.

The ALSA OSS plugin still needs packaging and error-propagation fixes: its
upstream pointer callback converts GETOPTR errors to pointer zero, and its drain
callback ignores SYNC errors. It also assumes the requested buffer geometry.
Do not enable this route until those errors propagate and the configured native
slave passes ALSA pointer/delay/drain acceptance. Default file-over-null ALSA and
Firefox launch settings remain unchanged; Firefox normal-launcher audio remains
open.

Sources: [Linux OSS pointer/drain implementation](https://raw.githubusercontent.com/torvalds/linux/master/sound/core/oss/pcm_oss.c),
[ALSA OSS plugin](https://raw.githubusercontent.com/alsa-project/alsa-plugins/v1.2.12/oss/pcm_oss.c).


## Fragment readiness and clocked ALSA slave (18 October)

Poll/epoll readiness now requires room for a negotiated fragment. The threshold
is stored with mixer geometry, so descriptor scans use one cached snapshot
without acquiring the per-open mutex or calling HDA. Writes can still accept a
smaller whole-frame request when appropriate. Reset preserves the negotiated
threshold; a new description starts with default geometry. A paused 4x1,024-byte
guest queue reports neither poll nor epoll readiness with 1,020 free bytes and
reports both at exactly 1,024. Hosted tests also cover in-flight accounting
across this threshold. The OSS trigger capability bit is corrected from 0x10
to the Linux UAPI value 0x1000; the guest endpoint regression checks it.

`tools/alsa-oss-pkg` builds a Brook playback slave from the pinned ALSA plugin
source. It propagates callback errors, negotiates native stereo S16_LE/44.1 kHz
with bounded buffers, and disables output while data is prepared. An outer plug
can perform client conversion, which needs separate non-native-format acceptance.
Prepare requires successful native GETOPTR support before using GETODELAY as a
clock, preventing fallback to the kernel's approximate unclocked legacy estimate.
The host callback harness exercises the patched source and real null-PCM params,
including errors and failed preparation, without opening a host audio device.

The first plugin candidate passed frequent clock queries but failed a whole-ring
gap: after 0.7 seconds without querying, DSP reported zero while ALSA still
reported 16,384 queued frames. Its modulo pointer could not distinguish no
movement from one complete ring. The corrected plugin uses ALSA boundary mode,
with completion derived from accepted application frames minus this stream's
actual incomplete frames. It tracks the expected application origin so
forward/rewind cannot manufacture progress. An empty zero-origin reset can
rebase; reset with pending data returns EPIPE on observation until prepare.
Private metadata is protected by a mutex; callers must serialize configuration
operations on one PCM handle. No complete ALSA concurrency/API claim is made.

In the corrected guest, both `brook_oss` and `brook_plug` pass 68 bracketed delay
comparisons against the same open DSP descriptor. A separately paused stream
remains full while another drains. Three whole-ring gaps per route now report
both zero delay and full ALSA availability. Raw real-library tests also cover
drained reset recovery, pending-reset/forward/rewind rejection and recovery,
and a partial mmap commit: 64 frames are charged while uncommitted bytes are not.

Both routes deliver all 9,660 expected nonzero frames in exact stereo order
through 30 short sounds and repeated prepare/drain. The host barrier releases
the source only after recording starts, and all source execution finishes within
capture. This checks actual PCM delivery in addition to API counters. It does
not certify glitch-free timing, all client conversions or browser playback.

Evidence: `build/workloads/alsa-oss-v2-20261018/`, kernel
`9261115b37d0605c2403348e80b801b12e3ccefdee0a93091ff1eeb728053675`,
plugin `/nix/store/dfpfpdlxhhnapm54xrqfakfyfm8wssqy-alsa-plugins-oss-brook-1.2.12`.
The plugin installs through the real guest `nix-install` as UID 1000. The initial
full-ring failure is retained in
`build/workloads/alsa-oss-20261018/alsa-full-wrap-baseline/`. All 34 hosted suites
and supported compiled-entry checks pass in
`build/alsa-oss-capability-build-20261018.log`; the plugin callback checks pass in
`build/alsa-oss-boundary-build-20261018.log`.

The corrected VM returns to ten tasks and its final two-second idle capture
contains zero PCM frames. Checked guest synchronization, launcher exit zero,
serial/device checks and all three offline filesystem checks pass. The candidate
keeps the installed test package for a subsequent isolated Firefox run; no
existing user image/profile was changed.

Default ALSA configuration and Firefox launch preferences remain unchanged.
Normal-launcher Firefox audio, IPC, non-native client conversion and broader
application acceptance are still open. The slave is intended as an ongoing
compatibility path; the generators and captures are regression helpers.

Sources: [Linux OSS capability definitions](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/soundcard.h),
[ALSA ioplug transfer and boundary behavior](https://raw.githubusercontent.com/alsa-project/alsa-lib/v1.2.15.3/src/pcm/pcm_ioplug.c).

## Firefox with the clocked ALSA route (18 October)

The isolated `build/workloads/firefox-clocked-20261018/` candidate cloned the
accepted ALSA v2 disks and frozen artifacts, with eight CPUs, 4 GiB RAM, CPU
composition and silent host audio. Session `f3bda22cb9f5427cb238542942a0bba0`
used kernel SHA-256
`9261115b37d0605c2403348e80b801b12e3ccefdee0a93091ff1eeb728053675`.
The current `brook-firefox` wrapper was staged byte-for-byte as `/boot/FFNORM.SH`;
the cloned installed profile lacked that wrapper. Firefox ran as UID 1000 against
the existing cloned `/data/home/.mozilla/brook` profile.

With only test ALSA configuration/plugin environment variables supplied, Firefox
still selected PulseAudio, remained at media time zero after Play, and produced
no PCM. It closed normally with exit zero and no remaining tracked job tasks.
After backing up the cloned `user.js`, adding only
`user_pref("media.cubeb.backend", "alsa");` allowed playback through the same
wrapper. No content-sandbox or cubeb-sandbox settings were changed; the audio
process log reports `media.cubeb.sandbox: true`. This does not establish complete
Firefox sandbox security or explain every child-process diagnostic.

The first short-loop capture contained nonzero PCM but repeated approximately
0.54-second silent intervals every five seconds. A separate, generated 30-second
H.264/AAC fixture removed looping from the measurement: its ten-second capture
contains 441,193 stereo frames over 10.0044 seconds, RMS 2046 in signed-16 units,
and no silent 10-ms RMS bins below one signed-16 unit. Positive zero crossings
over the interior eight seconds measure the generated 440-Hz tone at 440 Hz.
Evidence is in `alsa-continuous-audio2.json` and its referenced WAV;
`alsa-continuous-playing.png` records the video. This is a bounded steady-playback
check, not proof of glitch-free arbitrary media or acceptable loop transitions.

Native WM close returned the entire tracked Firefox job to terminal exit zero
with zero live tasks. A subsequent three-second capture has no active output;
the guest returned to ten processes. Checked guest sync, launcher exit zero,
stopped-process verification, serial/device acceptance and all three offline
filesystem checks pass. Startup stderr still includes crashreporter/telemetry
diagnostics, which are retained rather than treated as crash-free acceptance.

Normal shipped-launcher integration remains open: this run needed test-only
ALSA environment variables and a cloned-profile backend preference. The image
must supply the plugin/configuration and a preference policy that preserves user
choices, then pass the same test without diagnostic setup. Default ALSA settings
and the repository Firefox wrapper remain unchanged.

## Remaining integration acceptance

Review with Astra identified the following integration requirements. The native
receipt, completion, discontinuity, short-tail and native pointer/drain
prerequisites now have the coverage above. Fragment readiness and the native
ALSA slave also pass targeted checks; normal defaults, client conversion,
Firefox and non-native OSS mappings remain:

- Preserve the independently owned FIFO and serialized safe-staging foundation
  above while adding hardware accounting; do not reintroduce shared ownership.
- Account for accepted, hardware-committed and completed frames, including a
  pending delivery batch and partial/error driver writes. Track contributing
  stream generations through hardware completion.
- Preserve observable HDA progress and discontinuities across recovery; reject
  ambiguous cyclic LPIB polling gaps and start short finite sounds below the
  8,192-byte streaming threshold without requiring another writer call.
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

## Normal package and audio timeline checkpoint (18 October)

The candidate `tools/firefox-brook-pkg` now supplies the native OSS plugin and
44.1 kHz stereo plug configuration, replacing only the inherited legacy ALSA
configuration. Its ALSA backend setting is a default preference, preserving
explicit user choices. The public package index/image has not been promoted.

`audio stats` on the guest debug channel reports a bounded cached-mixer snapshot:
stream identity/reset epoch, submitted/completed frames, software/in-flight
queues, capacity, enable state and discontinuity. It separately samples the
stable guest clock; it neither polls the driver nor consumes samples. Identities
and epochs must match before subtracting counters across snapshots. Empty
software queues can still have in-flight hardware audio. One-second samples
cannot exclude shorter underruns, and these are not hardware interrupt-time
timestamps. The bridge exposes this optionally through
`vm_stats(audio_details=true)`; older kernels report `supported=false`.

The isolated `heap-sub-bins-audio-timeline-20261018` run compares a blocking
native OSS producer with one normally launched Firefox, without backend/config
environment overrides. Neither cloned `user.js` nor `prefs.js` has a cubeb
backend or sandbox override. With the silent host backend, the direct synthetic
440 Hz tone captures 441,319 frames (10.0072 seconds) over 10.0061 host seconds,
without silent 10 ms bins. The software-plus-in-flight queue remains supplied
at each one-second observation, with no recorded stream discontinuity.

Firefox captures 441,306 frames (10.0069 seconds) over 10.0070 host seconds.
An 80 ms silent interval occurs 0.41–0.49 seconds into this capture; the rest
has no silent 10 ms bins. Thus the earlier short PCM captures are not a
universal fixed-rate mismatch, but intermittent playback remains unaccepted.
Firefox's negotiated capacity is 4,096 frames versus the direct producer's
16,384; this is an observed buffering difference, not a proven cause. Its
one-second observations generally retain thousands of in-flight frames and
cannot identify the cause of that short gap. Browser telemetry now includes
page identity and sequence numbers so queued/out-of-order HTTP reports need
not be mistaken for playback resets.

Both producers exit zero with no remaining tracked tasks; both post-exit audio
captures are inactive. Checked shutdown, launcher exit zero, serial audit and
all three offline filesystem checks pass. The audio snapshot's non-consuming
and close/reuse behavior passes the production-mixer hosted test; all 36
current hosted suites, the supported entry audit and 40 bridge contract tests
pass. This does not establish YouTube audio, pause/resume, simultaneous-app
acceptance or fresh bootstrap package-closure coverage.

## Firefox YouTube stream setup — 18 October 2026

With the accepted userspace-clock kernel, normal Firefox still played YouTube
video without audio. A ten-second capture had zero PCM frames, mixer stats had
no streams, and serial output reported OpenCubeb stream-initialization failure.
The player selected AV1 video and Opus audio. This failure differs from an
underrun in an already running stream. The adjacent normal Chromium run produced
441123 frames over 10.0029 host seconds with no silent 10-ms bins. Both browser
lineages drained; heap, checked shutdown, serial audit and all three offline
filesystem checks passed. Evidence: build/workloads/vdso-youtube-20261018.

A standalone guest probe using Firefox's packaged ALSA configuration accepted
44.1-kHz S16_LE and FLOAT_LE streams but rejected both 48-kHz streams with
EINVAL during period negotiation. A Linux control using real ALSA and the
production OSS plugin against synthetic device operations reproduced this: its
30-case rate/format/latency matrix failed 20 cases. Power-of-two period and ring
requirements plus an integer count of periods prevented compatible resampler
geometry.

The plugin now accepts arbitrary logical period and ring sizes within the
existing bounds. Physical OSS fragments remain 256 bytes; sufficient whole
fragments cover the software ring with less than 64 stereo frames unused.
Application transfer, completion and delay accounting retain the logical ring
and actual accepted/queued frames. Logical capacity is never expanded merely
because physical storage has spare room. Callback checks and all 30 real-ALSA
negotiation cases pass in the Nix check phase. The final assertion-enabled
build's runtime PCM library is byte-identical to the installed guest candidate.

On fresh isolated disks, guest nix-install installed that Firefox closure as
Brook and the normal installed-app launcher selected it. The four-case guest
rate/format setup probe now passes. YouTube produced 441126, 443780 and 441212
frames in three consecutive approximately ten-second captures; their longest
silent 10-ms runs were respectively 29, zero and zero bins. Pause produced zero
frames and an inactive stream. Resume produced 441484 frames in 10.007 host
seconds without silent 10-ms bins. The tab also showed its audio indicator.
Intermittent initial gaps remain; this is not acceptance of indefinite smooth
playback or of every ALSA operation.

The candidate package remains separate from base-image/public-index promotion.
Evidence and exact store identities: [Firefox package checkpoint](../tools/firefox-brook-pkg/README.md),
build/workloads/firefox-rate-youtube-20261018, kernel
e05bc514d02aa3050148b3a779cfc889752bfd63d47f0f4e819a4862b0619602.

The corrected Firefox run also passed native close with no remaining browser
lineage tasks, inactive post-exit audio, heap integrity, acknowledged sync,
launcher exit zero, serial audit and all three offline filesystem checks.
