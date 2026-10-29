# Controlled memory-exhaustion recovery

Branch: `wip/oom-recovery-20261029`, based on accepted paging/compression.
The integrated worker is enabled by default; `opt/oom-recovery=off` is an
explicit diagnostic escape hatch. Bounded native acceptance passed; this is not a claim
that unrestricted exhaustion or every application boundary is supported.

## Recovery and ownership

A prestarted kernel worker observes sustained physical pressure below 8 MiB,
tries bounded file/executable-cache reclaim, and gives existing quarantine and
cleanup workers time to progress. User allocations preserve a 4 MiB kernel
floor; a separate 256 KiB emergency reserve can be released during pressure.
Allocation-failure hooks only update atomics after releasing the PMM guard.
They do not kill, wait or acquire scheduler locks. Ordinary allocation failure
returns remain intact; failures with healthy headroom are not global OOM proof.

Selection scores exclusive resident managed private pages, excluding CoW,
extra references, file/memfd/device mappings and cross-group shared address
spaces. Huge virtual reservations and swapped bytes do not inflate the score.
This conservative policy does not account comprehensively for GPU/shared-memory
consumers or aggregate a multiprocess browser into one application.

Registry references protect snapshots, bounded page-table batches release AS
locks between batches, and commit revalidates incarnation, AS identity and all
sharing users. Explicit kernel/critical-service roles are protected; UID 0 and
process names are not criticality heuristics. Contention/identity turnover has a
finite retry budget rather than being mistaken immediately for no eligible app.
New ordinary app launches are refused below the admission threshold with a
retained launch-memory event. Diagnostic jobs and critical-service startup keep
their existing control paths.

Termination uses the existing asynchronous fatal-signal machinery. An AS-wide
birth fence prevents prepared CLONE_VM children escaping. Selected tasks carry
an episode identity across exec, including changes of PID/group leader. The
worker releases temporary process/AS references before waiting. Completion
requires both real AS teardown (including TLB retirement) and retirement of all
episode-marked tasks; disappearance of the old group is insufficient. A
10-second cleanup deadline reports stalled recovery without selecting another
victim. After actual retirement, a separate drain phase waits for 32 MiB free
or at most 200 ms before recording headroom, allowing the normal quarantine
worker to progress. No second victim is selected during that phase. Its timeout
is not proof of drain failure: concurrent allocations may consume returned RAM.
Hysteresis and cooldown prevent rapid repeated selection.

## User-mode messages and Settings

`tools/brook-message-pkg` provides a GTK app with native decoration. A fixed,
versioned 424-byte ABI selects concise wording and expandable technical details.
`brook-message --watch` is started as a protected service before pressure. A
16-record allocation-free kernel queue retains events while memory is scarce;
syscall `0xB41` permits ordinary users to read only their own or broadcast events.
Presentation waits until at least 32 MiB is free. A bounded user queue coalesces
progress for the same victim while retaining unrelated messages.

Settings persists `[Experience] technical_details=true/false` alongside existing
preferences. Simple messages are the default; technical details are always
expandable. The preference changes presentation only. Both apps honor
`BROOK_USER_SETTINGS`, otherwise `/data/home/.config/brook/settings.ini`.
Manual examples are `brook-message --code app-closed-memory --app GIMP`,
`--event-file` for one ABI record, and `--print` for display-free formatting.

Technical text distinguishes unknown, disabled, configured and full experimental
paging. Paging currently covers opted-in mappings; logical occupied slots are
not a statement that every app can swap. Global before/after free RAM is not
claimed as memory independently recovered from the victim. The separate
released-bytes field is displayed only with a valid attribution flag; the worker
does not currently set it. MCP `vm_stats` includes the recovery phase, stable
victim identity, conservative score, observed headroom, failures and event cursor.

## Validation checkpoint

Both Nix UI packages build and package checks pass. Hosted tests exercise the
actual worker orchestration, allocator reserve/failure callbacks, bounded VMM
scoring, cache reclaim, destruction watch with delayed TLB acknowledgement,
scheduler selection/criticality, clone/exec races, immutable queue/UID isolation,
formatter and persistent preference. No-victim, stalled-cleanup and recovered-
during-selection cases are covered. Compiled entry/scheduler audits pass.

First native private test: 512 MiB VM, 8 vCPUs, Intel Venus, no paging backing.
The worker terminated the anonymous-memory test (PID/incarnation 34) at
6,578,176 free bytes. After real teardown, observed free RAM was 173,121,536
bytes. SIGKILL completion was collected through the original bridge job;
desktop, bridge and protected message service survived. The separate patterned
memory witness exited successfully; the retained event syscall and native UI
were verified. Settings saved the detail checkbox and launched successfully
after recovery. Guest sync, QEMU exit and original owner exit all completed
successfully; data/nix/tmp filesystem checks were clean. A clean reboot retained
the saved technical preference and opened the details by default. Native review
verified compact collapsed/expanded dialog sizing; the final build also removes
unwanted selection of the simple summary.

A second pressure run also preserved a 30-second OSS writer: 1,323,000 frames
completed in 30.018 guest seconds without a reported writer failure. This uses
a silent backend and is not physical-speaker or no-stutter certification. That
run exposed an early post-teardown headroom sample, motivating the bounded
drain phase above. Its first backing fixture did not retain full occupancy
through stdio logging (the experimental pager deliberately restores pages on
unaudited writev); it is not evidence of full-backing acceptance. The corrected
fixture uses scalar write, mixed compressibility and integrity verification.

Receipts and private screenshots live under ignored
`build/reviews/oom-recovery-20261029/`; build/host/audit logs use `build/oom-*`.


Final full-backing gate (512 MiB / 8 vCPUs, Intel Venus, default recovery):

- Disposable 4 MiB logical store fully occupied: 496 compressed-RAM slots and
  528 disk slots. The filler retained all 1,024 slots throughout pressure, then
  verified every original byte and released all slots successfully.
- Exactly one victim: anonymous-memory hog PID/incarnation 39. Physical free RAM
  was 6,537,216 bytes before selection and 138,412,032 after real retirement and
  the bounded drain opportunity. The original job collected SIGKILL.
- The independent patterned-memory witness survived and passed. The concurrent
  OSS writer completed all 1,323,000 frames in 30.015 guest seconds with no writer
  failure. This remains a silent-backend delivery check, not audible certification.
- UID-1000 reads through the real event syscall verified exactly the request and
  completion records, full configured experimental backing, actual headroom and
  absence of an independently attributed released-bytes flag.

The combined test caught and fixed an incomplete-scan bug: a small measurable
app could be selected while a larger app's AS gate was busy. Scoring now retries
its gate briefly without holding locks; any incomplete scan blocks commit and
has a bounded whole-scan retry budget. Persistent uncertainty produces a
no-victim event. Actual-worker regression tests cover a smaller measurable app
alongside a temporarily/permanently busy larger app. Astra reviewed this and the
post-retirement drain phase. The failing runs and their receipts are preserved.

Final native visual review verified the saved expanded-detail default, readable
520-pixel compact dialog, unselected simple summary and truthful full-backing
status. A subsequent ordinary-user Settings launch rendered successfully.
Checked guest sync, QEMU exit and original launcher exit completed with zero;
offline data/nix/tmp filesystem checks all passed. Final ignored evidence is
`build/reviews/oom-recovery-20261029/native-512-scan/acceptance.json` and its
referenced job/lifecycle/fsck receipts. Kernel SHA256:
`c8a10f938aed9efc8914158d8a9af797f3bef53cd3bb8810fbbefe250c270957`.

## Repeating the bounded workload

Run `nix-shell --run './scripts/build_oom_probes.sh'` to build static generated-
data-only probes. Stage those binaries onto private guest disks; never use a
mutable signed-in source image directly for exhaustion tests. Run `oom_witness`
and then `oom_hog` as the ordinary desktop user. The hog stops allocating near
7 MiB free and has a 60-second fallback; the witness checks a patterned 2 MiB
mapping for 60 seconds. Expected recovery collects SIGKILL for the hog and exit
zero with `OOM_WITNESS_INTEGRITY_PASS` for the witness.

For the full-backing gate, attach a fresh 4 MiB logical image made with
`scripts/page_store_image.py`, select its device with `opt/page-store-device`,
and set `opt/page-store-ram-mib=1`. Run `oom_backing_full` as explicit guest root
before the ordinary-user probes. Its ready marker must show both RAM and disk
slots; its final integrity marker and exit zero are required. It intentionally
uses audited scalar write/clock calls while holding backing full. Require a
subsequent ordinary app launch, lifecycle checked shutdown, original launcher
exit zero and offline filesystem checks before considering the run accepted.

Remaining limits: multiprocess-app aggregation, shared/GPU-heavy attribution,
application priority controls and broader real-driver/protected-cleanup stress
are follow-up work. Deterministic hosted tests cover stalled cleanup and no safe
victim; the native gates above do not claim exhaustive real-device failure testing.
