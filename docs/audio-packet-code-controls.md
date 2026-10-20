# Native coded-audio timing controls

A generated native OSS signal reproduces accelerated source progression in
QEMU timer-paced HDA, with unchanged pitch. The same kernel in callback mode
produces near-normal progression in these controls. This supports the emulated
audio-path lead, while the historical roughly 1.3× Firefox/Quake II report still
needs a clean application-specific reproduction and default-profile validation.

## Signal and decoder

`audio_timing.py --signal packet-code-v1` emits one distinct frequency every
100 ms. For packet index `i`, the frequency is
`400 + 40 * ((137*i + 29) % 463)` Hz. The permutation avoids confusing ascending
frequency with advancing source time. All packets have 4,410 stereo s16 frames
at 44.1 kHz, integer cycles and low amplitude. Up to 450 packets are generated
before opening/timing the device, so signal generation does not become a
producer-starvation workload. The original 440-Hz default is unchanged.

`analyze_audio_packet_code.py` uses 50-ms FFT windows with 10-ms hops, filters
weak/mixed windows and fits a small pitch correction to the frequency comb.
Pitch fitting does not use a desired source-time slope. It then decodes the
permuted IDs and reports symbol runs, backwards transitions, unobserved IDs and
an approximate source-time/PCM-time regression. NumPy is required. Rates use
stored PCM duration; the independent capture receipt supplies host duration.
WAV can omit inactive periods and does not expose individual delivery times.

Five permanent regressions use generated data and FFmpeg transforms: normal
progression, known 1.3×/0.5× pitch-preserving tempo, a five-symbol omission,
a five-symbol replay, a 1.005× pitch/rate change, and rejection of a plain tone.
The known tempo estimates are 1.30023× and 0.50003×. The pitch control also
misses some symbols, demonstrating why “unobserved ID” is not proof of transport
loss. Capture coverage, pitch fit, transients and raw runs remain part of review.

## Native results

Each fresh private VM has eight vCPUs, six GiB, MLFQ, the unchanged per-vCPU-clock
kernel and 13 ABI-34 modules, host affinity 0–13, SDL using the private 44.1-kHz
PipeWire null graph, and a 20-ms requested host buffer. The two codec modes are
checked with QMP. Each emits 30 seconds of coded audio idle and with 24 bounded
CPU workers (12 seconds). An eight-second PCM capture is taken during each
probe; the loaded capture follows the worker READY marker and a 1.1-second wait.

| HDA mode / workers | 30-second guest completion | Source seconds / PCM second | Pitch scale | Decoded runs | Unobserved IDs / backwards transitions |
| --- | ---: | ---: | ---: | ---: | ---: |
| Callback / 0 | 29.952 s | 1.00014 | 1.0000 | 80 | 0 / 0 |
| Callback / 24 | 30.005 s | 1.00014 | 1.0000 | 80 | 0 / 0 |
| Timer / 0 | 26.815 s | 1.10468 | 1.0000 | 87 | 1 / 0 |
| Timer / 24 | 26.886 s | 1.11962 | 1.0000 | 88 | 1 / 0 |

Both callback captures contain 637 decoded windows out of 796, with consecutive
IDs. Their different artifact paths and SHA256 hashes are retained even though
the fitted rates and window counts happen to match. Timer captures advance
through more source symbols without a fitted pitch increase; that is consistent
with shortened/omitted source intervals, rather than establishing which layer
loses data. The single unobserved ID per timer capture is not independently
certified as a dropped complete packet.

The PCM-duration/host-duration ratios are 0.99975/0.99942 for callback and
0.99701/0.99958 for timer. RPC-bracketed guest-clock bounds are broad: roughly
0.93–1.065 overall. Timer-specific upper bounds are 1.0391 idle and 1.0599 loaded,
below their observed source/PCM progression. These brackets are consistent with
1× and exclude the full observed acceleration as one constant guest-clock factor;
they do not prove tight instantaneous clock calibration. Raw KVM/steal runtime
counters remain free of invalid/backwards/clamped/stale/saturated samples.

After each pair, no app streams remain and a later one-second capture contains
zero frames, covering the previous residual-buffer concern. The two captured channels are bit-identical for the generated dual-mono signal
in all four captures; decoding the first channel therefore covers the same
signal in the second. All four probes
complete exactly 1,323,000 frames; tone/load groups drain. Original wrapper exits,
routing watchers, heap, four offline filesystems and twelve-document/four-profile
preservation checks pass. The default scheduler, HDA mode and host buffer stay
unchanged. These are one trial per mode/load combination, not a statistical or
physical-listening acceptance.

## Transport failure retained

The first full-result export used stdout for a 173,280-byte report. Its guest
process completed zero, but the bridge reported a lost prefix, so that export
is unaccepted. The same callback VM was retained. Small guest size/SHA256
receipts and post-shutdown read-only private-disk extraction recovered both
reports exactly. The remaining timer controls use this route from the outset.
No audio probe was replaced to erase the transport failure. A live guest-file
artifact export is [backlogged](vm-bridge-next.md).

Ignored evidence is under
`build/workloads/timing-scheduler-20261020/packet-audio-controls`,
`packet-code-validation`, `packet-code-tests.log` and the separate small-buffer
timer browser cohorts. Both Linux and Brook timer browser controls advance media
about 5–6% fast with the 20-ms buffer. The Linux browser's strict page-clock
anchor model rejects this particular trial; its original rejection and separate
server-causal bounds are retained. That result is not promoted to clock-model
acceptance by the successful native probes.

The [Quake II/music control](q2-music-timing-control.md) completes the GL3 demo
near nominal time, but its music-only repeat exposes a native stream
discontinuity and low active audio coverage. It is not continuity acceptance.
Next steps are identifying that fault's reason, a separately traced emulated
HDA run to correlate source gaps with occupancy corrections, and bounded host
contention. The original recording's larger factor and any remaining application
or scheduler-specific contribution remain open.
