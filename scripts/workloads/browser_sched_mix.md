# Brook browser/audio/scheduler cohort

Use an owned private review VM, eight vCPUs, six GiB, boot-selected RR or MLFQ,
and the isolated silent 44.1-kHz audio sink with diagnostic callback HDA mode.
Run one VM at a time with host CPU affinity 0–13. The controller deliberately
requires the review's `candidate.json` to record `private_clones: true`,
`memory_gib: 6`, `vcpus: 8` and `hda_use_timer: false`; `session.txt` identifies
the actual boot. Verify these against the launch arguments before measurement.

Serve an existing generated 120-second H.264/AAC MP4 on loopback. Put its MP4
metadata at the front (`-movflags +faststart` when creating/remuxing it). No
clip or game assets are bundled. A clip that must loop/seek during the CPU
interval is a different cohort, as is a metadata-at-end or prefetched fixture.

```sh
python3 scripts/workloads/serve_browser_sched_mix.py \
  --output build/browser-fixture --clip /absolute/path/to/generated-faststart.mp4
python3 scripts/workloads/run_browser_sched_mix.py \
  --review build/reviews/private-review --fixture-directory build/browser-fixture \
  --output build/browser-results/rr-0 --phase rr-0 --policy rr
```

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

Keep HTTP acknowledgment, input-to-DOM, RAF opportunity and physical presentation
separate. Clock-anchor intersection assumes a verified 1x page/host clock and
allows 2 ms browser timestamp quantization. WAV capture omits periods when no
samples are delivered: compare frame count/sample rate with host duration,
not only stored silence. Report trace gaps, buffered-ahead/ready state, media
progress, CPU throughput/fairness and latency distributions. A silent sink does
not establish audible quality. Never pool different fixture/backend/memory/
visibility cohorts or advertise a single pair as statistical acceptance.
