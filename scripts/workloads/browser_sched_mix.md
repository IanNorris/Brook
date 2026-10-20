# Brook browser/audio/scheduler cohort

Use an owned private review VM, eight vCPUs, six GiB, boot-selected RR, MLFQ or EEVDF,
and the isolated silent 44.1-kHz audio sink with diagnostic callback HDA mode.
Run one VM at a time with host CPU affinity 0–13. The controller deliberately
requires the review's `candidate.json` to record `private_clones: true`,
`memory_gib: 6`, `vcpus: 8` and `hda_use_timer: false`; `session.txt` identifies
the actual boot. Verify these against the launch arguments before measurement.

`--hda-use-timer off` is the diagnostic default; `on` selects a separate
prepared timer-mode cohort. The controller verifies the live QOM property and
records it in `identity.json`. Both eight-CPU pvclock and steal masks must be
255 for its runtime gate. Mode/backend comparisons must retain their own results.
For SDL into the private graph, start routing when the bridge session appears,
before guest-readiness polling. A resumed watcher can adopt existing validated
links. Preserve the original launcher even if readiness observation expires.

Serve an existing generated 120-second H.264/AAC MP4 on loopback. Put its MP4
metadata at the front (`-movflags +faststart` when creating/remuxing it). No
clip or game assets are bundled. A clip that must loop/seek during the CPU
interval is a different cohort, as is a metadata-at-end or prefetched fixture.

```sh
python3 scripts/workloads/serve_browser_sched_mix.py \
  --output build/browser-fixture --clip /absolute/path/to/generated-faststart.mp4
python3 scripts/workloads/run_browser_sched_mix.py \
  --review build/reviews/private-review --fixture-directory build/browser-fixture \
  --audio-route-directory build/browser-audio-routes \
  --output build/browser-results/rr-0 --phase rr-0 --policy rr
```

Before running the controller, start `scripts/route_silent_vm_audio.py` for the
actual bridge session and the private daemon's `daemon.json`, writing evidence
to `build/browser-audio-routes`. Wait for its `routed.json` while keeping the
observer alive. The isolated daemon has no session manager: a running null sink
alone does not connect QEMU's output. See the [reference instructions](../../tools/linux-workload-reference/README.md).

The server binds a fresh loopback port, writes it to `server.json`, copies the
page and links the supplied clip. It stops after one hour or when its output
folder contains `stop`. The controller starts a fresh Firefox profile as UID
1000, checks a VM keyboard preflight, measures idle playback, runs 24 CPU workers
for 30 seconds, captures eight seconds of VM output audio, sends four VM-only
keyboard probes and exports the page's bounded trace. It closes Firefox and
requests guest shutdown on success. The caller must still wait for its owned
launcher, verify authoritative QEMU exit, inspect serial/heap results, run offline
filesystem checks and verify saved-user-data preservation.

On an observation timeout, inspect the existing receipts/job/VM. Re-running with
the same output directory resumes known tracked operations; it never creates a
replacement job merely because observation timed out. An unresolved input or
capture intent stops automatic continuation. Explicitly resolve that evidence
before proceeding. Browser errors do not automatically close a VM under review.

Reanalyze saved trace evidence without a running VM:

```sh
python3 scripts/workloads/analyze_browser_sched_trace.py \
  build/browser-results/rr-0 --output build/browser-results/rr-0/buffered-analysis.json
```

The controller also saves before/after runtime and policy-service snapshots.
A clock/runtime anomaly fails the run before shutdown, preserving the same guest
for diagnosis. Inspect the per-CPU registration masks as well as the zero-error
counters. These safety gates do not declare playback smooth: near-zero video
callbacks or missing PCM delivery are valid negative performance evidence.

For repeated policy comparisons, use fresh clones/profiles, the exact same
kernel/modules/clip/controller and explicit audio routing. Run one VM at a time;
reverse policy order on the second block and retain per-run host load/memory
samples. Two repeats support ranges and consistency checks, not confidence
intervals or a default-policy decision. Analyze the buffered trace when HTTP
arrival is too delayed to yield causal loaded samples.

Keep HTTP acknowledgment, input-to-DOM, RAF opportunity and physical presentation
separate. Clock-anchor intersection assumes a verified 1x page/host clock and
allows 2 ms browser timestamp quantization. WAV capture omits periods when no
samples are delivered: compare frame count/sample rate with host duration,
not only stored silence. Report trace gaps, buffered-ahead/ready state, media
progress, CPU throughput/fairness and latency distributions. A silent sink does
not establish audible quality. Never pool different fixture/backend/memory/
visibility cohorts or advertise a single pair as statistical acceptance.

For a separate diagnostic cohort, add `--profile-ms 2000` (allowed 100–5000).
After the CPU workers report readiness, the controller snapshots process names
and starts one bounded CPU/context-switch capture. It exports that capture
after the CPU job drains, saving producer counts/losses and the exact-kernel
Speedscope artifact. Unknown start delivery is retained rather than retried.
IRQ-masked intervals are not uniformly sampled, context-switch counts are not
CPU service, and profiler disk writes/recording can perturb the workload. Keep
these results separate from ordinary performance controls; `summary.json`
records `profile_ms` explicitly.


For separate host-buffer experiments, Brook accepts `BROOK_AUDIO_BUFFER_US`
(default 200000 microseconds); the tested SDL override is 20000. Record the
value with the backend and codec mode. The Linux reference reproduced the
large-buffer slowdown, while smaller-buffer runs improved both guests; this
is an experimental configuration, not a default change or a completed diagnosis.
See [matched buffer controls](../../docs/linux-audio-buffer-control.md).

A second analysis can retain server-causal bounds without assuming a 1× page
clock:

```sh
python3 scripts/workloads/analyze_http_media_rates.py   build/browser-results/rr-0 --output build/browser-results/rr-0/causal-media-rates.json
```

This complements the buffered-trace model; it does not turn rejected clock
anchors into acceptance. Loaded selection inherits the supplied CPU interval's
assumptions, and delayed POSTs may leave intervals too wide for a useful result.
