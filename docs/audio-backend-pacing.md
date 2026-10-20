# Backend-specific audio pacing follow-up

The historical recording [matches roughly 1.3× tempo](playback-recording-rate.md),
but current audio results depend strongly on backend and HDA mode. Keep these
cohorts separate. Neither the default scheduler nor default HDA mode changes.

All tests here use kernel
`a9627f6fb5fcb7fd1f976f5c03138a03c68396f6c8ede195bef8ad4ec372d7a1`,
eight vCPUs, six GiB, MLFQ, a fresh Firefox profile and the same generated local
clip. SDL's audio driver is explicitly PipeWire, connected only to the private
44.1-kHz null sink; this is not a reproduction of the user's physical-speaker
SDL configuration. The browser controller now checks the live QOM codec
`use-timer` property against both its argument and preparation receipt.

| Backend / HDA mode | Buffered loaded media/page rate | Video callbacks/s | Captured PCM duration / host time | Launch acceptance |
| --- | ---: | ---: | ---: | --- |
| PipeWire / timer | 0.9747 | 6.64 | 0.9829 | Clean |
| SDL→private PipeWire / timer | 1.0370 | 6.52 | 0.9705 | Recovered diagnostic only |
| SDL→private PipeWire / callback | 0.5012 | 4.34 | 0.9996 | Clean |

The recovered SDL timer run contains a 6.23-page-second interval advancing
7.893 media seconds (1.2669×), with playbackRate 1 and no unplayable, muted,
ambiguous or over-two-second-gap samples in that interval. Two overlapping
six-second windows exceed 1.25×. This is a lead, not a clean reproduction of the
historical symptom: the initial boot observer timed out, audio links were
established later, and early window rates use the page clock. Later guest/host
brackets are consistent with approximately 1×. A fresh timer-mode SDL run with
early routing and independently bracketed startup clocks is still required.
The PipeWire timer run has no qualifying six-second window above 1.019×.

Callback SDL's cached native mixer completion counter advances at 0.5001 of
44,100 frames per guest second during capture, while its PCM recording contains
352,832 frames across 8.004 host seconds (0.9996× nominal duration). The media
clock also advances at roughly half speed. Dominant-tone FFT estimates from
central four-second PCM windows are 440 Hz for PipeWire callback and timer,
442 Hz for recovered SDL timer, and 439 Hz for SDL callback. An initial positive
zero-crossing estimate of about 220 Hz on callback SDL was misleading and must
not be reported as proof of half-pitch output. A constant tone cannot distinguish
all forms of chunk repetition, skipping or clock/receipt disagreement.

These mismatches do not yet locate the defect in Brook, QEMU/SDL, or the private
routing setup. In QEMU's timer codec path, virtual time drives DMA progress and
backend occupancy adjusts the timer origin; full codec buffers can be discarded.
The callback path differs. See the primary
[QEMU 10.2.2 HDA implementation](https://github.com/qemu/qemu/blob/v10.2.2/hw/audio/hda-codec.c).
A matched Linux control with the same private SDL backend is the next useful
separation of guest and host behavior.

## Recovery and verification

The first SDL VM reached Brook's desktop, although its readiness observer failed.
The bridge redirects serial output into its session directory: absence of the
review-directory serial file did not mean the guest had not booted. Captured
host threads, all eight guest CPU register snapshots, serial output and later
successful uptime reads are retained. No replacement VM was started to erase
that failure; measurement continued on the same instance.

Attaching the audio watcher after a one-shot route configuration exposed a
separate helper bug: duplicate `pw-link` returned “File exists.” `SilentRoutes`
now adopts only an unambiguous existing link between the validated QEMU source
ports and private-sink target ports. It rejects duplicate/failed matches and
creates only missing channels. Five Python regressions pass; a live watcher
successfully adopted both existing channels. This changes helper recovery,
not audio format or default routing policy.

The recovered browser and CPU lineages drain and guest sync is acknowledged.
QEMU becomes terminal, and all four preserved private filesystems pass read-only
checks. The original outer launch observer exited one and lost its wrapper
handle, so the original wrapper exit is unverified. No zero-exit receipt is
invented and this run is excluded from clean-launch acceptance. Its failed
readiness and routing attempts remain beside the recovered evidence.

The fresh callback SDL run starts routing as soon as the bridge session exists,
before waiting for guest readiness. Its launcher retains the original Popen
even if readiness observation fails. In this run readiness, both routing links,
clock/runtime gates, app drain, heap, guest sync, original wrapper exit zero,
four offline filesystem checks and twelve-document/four-profile preservation
all pass. The same clean gates pass for the PipeWire timer cohort.
No native Quake II SDL/callback result was obtained in this follow-up; earlier
Quake II timing controls used a different backend or kernel cohort.

Ignored evidence is under
`build/workloads/timing-scheduler-20261020/{timer-mode-browser,sdl-callback-browser}`,
with `audio-backend-cohorts.json`, `audio-backend-tone-peaks.json`,
`media-rate-windows.json` and the frozen `audio-backend-sources` manifest.
The recorded controller versions differ only in a strengthened registration-mask
gate; their exact source hashes and both variants are preserved. The external
verifier requires both masks 255 for all three runs.

Next controls should use the existing Linux image with the same SDL/private
route and both codec modes. A generated packet-coded signal, rather than one
constant 440-Hz tone, would allow PCM capture to reveal repeated/skipped source
intervals directly. First establish those controls before adding QEMU event
tracing or changing a default.
