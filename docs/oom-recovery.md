# Controlled memory-exhaustion recovery

Branch: `wip/oom-recovery-20261029`, based on accepted paging/compression.
The integrated worker is initially opt-in with QEMU firmware option
`opt/oom-recovery=on`. Native acceptance is in progress; this is not a claim
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
victim. Hysteresis and cooldown prevent rapid repeated selection.

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
desktop, bridge and protected message service survived. A separate patterned
memory witness, UI/persistence, later app launch, checked shutdown/filesystems,
and exhausted-backing acceptance remain to be completed before default enable.

Receipts and private screenshots live under ignored
`build/reviews/oom-recovery-20261029/`; build/host/audit logs use `build/oom-*`.
