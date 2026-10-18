# Package transfer and extraction

The 18 October clean bootstrap spent 732 seconds installing its SDK, compared
with 51 seconds compiling Brook. A component test found a correctness bug in
virtio-net's transmit ownership behind much of the network-fed extraction cost.

## Controlled decomposition

The probe uses the cached LLD 18.1.8 package from the bootstrap SDK: a 12,511,000
byte zstd archive containing a 38,807,024 byte NAR. It verifies compressed and
uncompressed Nix hashes before boot, injects both files into a private data disk,
and compares extracted file hashes and symlink targets with a host-restored
reference tree. Download and copied-file checksums must also match.

Each eight-vCPU, 4-GiB VM uses private host RAM disks and the prepared SDK seed.
Two rounds run in opposite order. The kernel ELF and every other driver are
unchanged between the following control and candidate; only virtio-net differs.
The static monotonic timer binary is byte-identical. No CPU profiler or packet
capture runs during these two-round measurements; ordinary bridge observation
and block-counter collection remain enabled. Host cache/thermal state is not
controlled, so these are local matched measurements, not a general throughput
claim.

| Stage, guest monotonic seconds | Before, rounds 1 / 2 | TX ownership, rounds 1 / 2 |
| --- | ---: | ---: |
| Read local NAR | 0.141 / 0.042 | 0.108 / 0.041 |
| Copy through a pipe to null | 0.338 / 0.299 | 0.304 / 0.292 |
| Local copy with fsync | 0.184 / 0.121 | 0.175 / 0.114 |
| Local decompression to null | 0.267 / 0.372 | 0.242 / 0.379 |
| Local plain-NAR extraction | 0.112 / 0.109 | 0.211 / 0.108 |
| Local decompression + extraction | 1.106 / 1.107 | 0.878 / 1.035 |
| Network decompression to null | 8.932 / 8.875 | 1.842 / 1.458 |
| Network decompression + extraction | 26.849 / 25.620 | 4.466 / 4.227 |

Network-fed extraction took about 83% less time. Direct downloads vary: control
null-output runs took 3.533 and 0.096 seconds, while file-output runs took 0.430
and 0.391 seconds. This variance is why a single download or CPU sample was
insufficient to identify the problem. Local disk operations were much faster
than the stalled package pipeline; these measurements do not justify disk
compression as the first fix for this workload.

Separate instrumented packet captures showed 15 incoming-data gaps of at least
50 ms in the old network-fed extraction flow, several lasting approximately
two seconds. Its wire payload included retransmissions. The candidate had no
such gaps in that flow and exactly one archive body plus HTTP headers on the
wire. Neither capture advertised a zero receive window. Packet-gap analysis
alone is not causal proof; the driver race and controlled module replacement
provide the additional evidence.

## Driver ownership

Virtio-net had one shared TX DMA buffer and descriptor zero, but no lock around
preparation, publication and completion. The receive poller and socket callers
can transmit concurrently. A second producer could overwrite an in-flight
ACK/frame or publish descriptor zero twice. The driver now holds a plain
spinlock through completion. Interrupts remain enabled; the current RX ISR only
wakes the poll thread, and TX callers run in process or early-boot context.
Adding an IRQ-context TX caller would require rechecking this contract.

An invalid completion ID/index advance quarantines the queue: shared DMA
storage remains untouched and later sends fail. A polling timeout instead
keeps descriptor zero pending and returns EAGAIN. Subsequent sends first check
for its valid completion, without modifying or republishing the pending frame.
Only after that completion can a new frame reuse the DMA storage. Driver reset
for a permanently unresponsive or corrupt device remains unimplemented.
Releasing the software lock is not evidence that the device released a timed-out
buffer. This follows the
[virtio device-cleanup ownership requirements](https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html).

`test_virtio_net_tx` includes the actual driver with a fake device notification
hook. It pauses the first DMA submission while another producer enters, checks
eight competing producers, ring-index wraparound, oversized input, pending DMA
preservation, valid late completion and bad-completion quarantine (including
late corrupt responses). Removing serialization makes the fixture fail;
restoring it passes. The host fixture uses a shorter spin budget to exercise
timeout ownership, not to establish real-device timeout latency. All 39 hosted
suites, the supported compiled entry audit and nine bootstrap observer tests
pass. This does not validate a physical NIC or add IRQ-context TX support.

## Reproduction and evidence

After building Release with no VM live, run inside `nix-shell`:

```sh
python3 scripts/package_pipeline_acceptance.py \
  --seed build/experiments/seeds/bootstrap-prepared-parity-20261014 \
  --narinfo build/package-cache/4cxwfdassdmaq3v4jrd7704mvclaamjb.narinfo \
  --output build/workloads/package-pipeline-new --instance 247
```

Use a new output directory and an available instance. `--pcap` instruments a
separate run with a private QEMU packet capture; analyze it with
`python3 scripts/analyze_pipeline_pcap.py RUN_DIRECTORY`. Packet payloads are not
printed by the analyzer. Successful disposable RAM images are released after
checked shutdown and offline checks; `--save-images` archives them instead.
Failures retain their VM, command identity and disks for diagnosis. An expired
observation does not authorize another launch.

Evidence under ignored `build/workloads/`:

- `package-pipeline-monotonic-20261018`: accepted two-round control.
- `package-pipeline-tx-fixed-20261018`: accepted two-round candidate.
- `package-pipeline-pcap-20261018` and `package-pipeline-tx-fixed-pcap-20261018`:
  separate packet captures and summaries.
- `virtio-net-tx-negative-20261018.log`: serialization removed, fixture fails.
- `virtio-net-tx-negative-and-restored-20261018.log`: restored fixture passes.
- `virtio-net-tx-build-suites-20261018.log`: build/audit and hosted suites.

All accepted runs require complete command output, exit zero and zero remaining
lineage tasks, content checks, heap integrity, checked shutdown, authoritative
QEMU exit, serial audit and offline FAT/ext2 checks. This component result alone
does not renew full bootstrap acceptance or prove browser startup/playback gains.

The preliminary `package-pipeline-baseline-20261018` is rejected timing evidence:
Bash's realtime-based clock went backwards. A separate three-second audit saw
three backwards realtime steps, up to 999,000,322 ns, while monotonic time did
not regress. Its content checks and checked shutdown/fsck passed. The RTC/pvclock composition bug was subsequently
[repaired and checked separately](realtime-clock.md); benchmark durations
use `tools/io-bench/monotime.c`, not realtime.


## Full SDK acceptance

Both full bootstrap paths subsequently passed at `610592d` with all 39 native
suites running three times per path, including under each guest-built kernel.
The clean seed installed all 91 SDK additions in 236 seconds, versus 732 seconds
in the preceding control (about 68% lower). Host observer intervals support the
same reduction. Compilation stayed around 50 seconds. See
[full acceptance](bootstrap.md#matching-39-suite-acceptance-after-tx-ownership-2026-10-18)
for source/fixture identities, stage timings, checked shutdown and filesystem
evidence. Real browsers have not yet been remeasured with this driver.

## Late TX completion follow-up (18 October)

The native review session `1b989704dd774a4ba955e42149b3684c` lost
transmission while both browsers were open. Chromium reported
ERR_NAME_NOT_RESOLVED and Firefox stopped/reload failed. Serial recorded
`virtio_net: TX timeout; queue quarantined`, followed by failing TCP sends and
unanswered DNS queries. A read-only QMP snapshot found the device used index
7215, matching the published available index, while the driver shadow remained
7214 and its fault flag stayed set. The device counter had advanced after the
bounded wait; that alone does not validate the completion element's ID.
The old policy could never examine a late completion and resume transmission.
Evidence is retained below
`build/workloads/exec-permissions-20261018/native/network-stall/`.

The pending-completion path above removes that permanent shutdown on polling
timeout without returning DMA ownership early. Invalid completions still
quarantine the queue. Deterministic host tests exercise no completion over
repeated sends, unchanged DMA bytes/one publication while pending, valid late
completion followed by another send, and invalid late completion IDs/advances.
The spin limit remains a bounded poll count, not a measured wall-clock timeout.
There is still no reset for a device that never completes.

Candidate `build/workloads/net-late-completion-20261018/review/`, session
`bd2e7088bf874fb58cf5b0e59a5a2413`, used eight CPUs, 8 GiB and frozen kernel
SHA-256 `560806163e48c379e508f70b041aa7fa2e9732b8f46f1a9dafb405aa8f1f5c48`.
With both browsers open, eight workers completed 32 local HTTP downloads,
verified all 86,109,536 bytes by SHA-256 and resolved Google, Amazon and Wikipedia
in 9.153 guest-monotonic seconds. This is a correctness/load probe, not a paired
throughput benchmark. Four subsequent network/debug checks succeeded. No TX
poll timeout occurred in this candidate run; late recovery is covered by the
host fixture, not claimed as exercised in this guest run.

Firefox supplied 804 playback samples and 2,007 total frames. Chromium stayed
at its first controlled-video frame despite play attempts; its playback is not
accepted by this check and needs a separate follow-up. Ctrl+T opened a Chromium
tab without adding a desktop window, and Super+Enter added one Terminal window.

All 40 native suites and the compiled entry audit pass. The tracked network
command exited zero with complete stdout/stderr and both launch lineages empty.
Both browser UI launch lineages drained after normal close, heap integrity was
OK, audio was inactive, checked sync succeeded, QEMU exited and the original
launcher returned zero. All three read-only filesystem checks passed. Five
saved documents retained exact hashes and UID 1000/GID 100. The stopped-run
serial audit found no kernel/device/user fault. See the retained
`build/workloads/net-late-completion-20261018/acceptance-audit.json` and
`build/net-late-completion-build-20261018.log`.

The failed native review was shut down separately: normal terminal commands
terminated the browsers and returned success from `sync`, visibly recorded in
`final-sync.png`, before QMP quit. Its launcher returned zero and all three
read-only filesystem checks passed. Its browser exit was forced and untracked;
those disks remain separate from the accepted candidate seed. A proposed live
kernel-memory recovery was rejected by automatic approval review and was not
executed. The normal terminal sync provided a safer shutdown route.
