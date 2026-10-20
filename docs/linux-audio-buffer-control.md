# Matched Linux and Brook SDL buffer controls

The Linux reference reproduces the SDL callback slowdown, and a smaller
requested host buffer improves it on both operating systems. This is a useful
experimental configuration, not a proven diagnosis of the original intermittent
1.3× symptom or a change to production defaults.

The controls use the same QEMU 10.2.2 executable and graphics libraries,
eight vCPUs, six GiB RAM, host affinity 0–13, generated 120-second clip and static
24-worker/30-second CPU benchmark. SDL uses its PipeWire driver with the same
explicitly linked private 44.1-kHz null graph. Linux uses the existing pinned
NixOS image, Weston and the documented Firefox software-vsync workaround;
Brook uses MLFQ. Those compositor/policy differences remain part of the cohort.
No host desktop input/capture or physical speakers are used.

## Server-causal measurements

The large-buffer Linux callback run's strict 1× page/host anchor intersection
fails by about 91.5 ms. It remains `inconsistent_clock_anchors`; its tolerance
is not widened to make it pass. The media slowdown can instead be bounded by
server timestamps: each sample follows the server's `/clock` timestamp and
precedes receipt of that sample's `/state` POST. Differences between two such
intervals bound elapsed host time without assuming the page clock advances 1×.

`scripts/workloads/analyze_http_media_rates.py` preserves those bounds and
rejects contradictory or overlapping intervals instead of manufacturing a rate.
It uses one page epoch and sequence order, retaining delayed HTTP delivery.
Five regressions include a deliberately fast page clock, reordered late posts,
uncertain CPU-load edges, bad receipts and mixed epochs. The existing buffered
trace analyzer remains separate. Its loaded-interval conversion still has its
own guest/host-clock assumptions; the new whole-run bounds do not depend on it.

| Linux SDL mode / requested buffer | Whole-run media/host rate bounds | Captured PCM duration/host time | Nonzero captured 10-ms bins |
| --- | ---: | ---: | ---: |
| Callback / 200 ms | 0.46835–0.46848 | 0.99984 | 448/800 |
| Timer / 200 ms | 1.01903–1.01960 | 0.99933 | 703/800 |
| Callback / 20 ms | 0.99815–0.99898 | 0.99984 | 800/800 |

The 20-ms Linux run also passes the strict buffered-clock model. During the
loaded interval, media advances at 0.99983× page time with 23.10 callbacks/s,
24.01 total video frames/s, zero additional dropped frames and no sampled
buffer starvation. A retained page `error` event with empty detail occurs while
readyState remains 4; later samples continue progressing. Do not call the
entire browser/network path error-free based on the successful measured window.

The native Brook comparison changes only the requested SDL host buffer from
200 ms to 20 ms, keeping callback mode, MLFQ, the same kernel/modules and early
private routing. Buffered loaded media progress improves from 0.50125× to
0.96420× page time; callbacks improve from 4.34/s to 7.02/s. The captured native
mixer completion rate improves from 0.50008× to 0.97594× its nominal frame rate.
The new PCM capture contains 351,936 frames over about eight host seconds
(0.99601× duration). This is successful delivery with remaining stalls, not
perfect audio or a physical-listening acceptance. Callback opportunity is not
physical screen presentation.

These are single controls for each configuration, not statistical acceptance.
The shared parameter dependence narrows the investigation to interactions with
the host audio path and buffering. It does not prove whether QEMU buffering,
SDL/PipeWire conversion, guest DMA geometry or another interaction is responsible.
QEMU's SDL backend passes the requested buffer size to SDL and processes audio
callbacks; see its primary
[10.2.2 implementation](https://github.com/qemu/qemu/blob/v10.2.2/audio/sdlaudio.c).
A packet-coded generated signal would identify source intervals lost/repeated
in captured PCM more reliably than the current constant tone.

## Configuration and verification

- Brook: `BROOK_AUDIO_BUFFER_US=20000` selects the tested override. The default
  stays 200000. Values are restricted to 1000–1000000 microseconds and normalized
  to decimal before reaching QEMU. The HDA mode and selected backend remain
  independent settings.
- Linux reference: `--audio-backend sdl --hda-use-timer off --audio-buffer-us 20000`
  requires the private audio daemon. The defaults remain PipeWire/callback with
  a 200-ms requested buffer. Actual argv, backend, timer mode and buffer value
  are recorded; the workload additionally verifies the live QOM timer property.
- `clock_state.py` reads monotonic/raw/boottime/realtime clocks and query-only
  Linux x86-64 `adjtimex` state. Three idle snapshots after the small-buffer
  Linux workload show `kvm-clock`, zero guest frequency adjustment and 10000-us
  tick. Host frequency adjustment is about +16.7 ppm. This does not establish
  the cause of the earlier large-buffer run's rejected page-clock anchors.

All three Linux VMs drain their tracked SSH jobs and Firefox, exit zero through
the original owned launcher, show serial powerdown and pass offline ext4 checks.
The new Brook run drains both tracked lineages, passes heap checks and has zero
invalid/backwards/clamped/stale/saturated clock/accounting samples, with pvclock
and steal masks 255. Its original launcher exits zero, four private filesystems
pass read-only checks and the twelve saved documents/four original profile trees
match the checked source. The core kernel was not rebuilt for these controls.

Evidence is under `build/linux-workload-reference-20261020/audio-backend-controls`
and `audio-small-buffer-control`, and
`build/workloads/timing-scheduler-20261020/sdl-small-buffer-browser`.
The earlier recovered SDL timer trial remains a failed launch acceptance as
[recorded separately](audio-backend-pacing.md). Nothing here turns that failure
into a clean result. A clean small-buffer timer-mode comparison, packet-coded
capture and native Quake II/music validation remain useful next steps.
