# Browser memory checkpoint — 17 October 2026

The main ELF loader now allocates the union of page-aligned `PT_LOAD`
segments rather than the entire virtual span between the first and last
segment. Holes have neither physical pages nor Image mapping ranges. Shared
segment boundary pages are allocated once; file copying and BSS initialization
retain their existing ordering. The image's recorded virtual span and program
break are unchanged. Segment permission hardening is separate work.

The installed Chromium ELF has a 500,654,080-byte virtual span but only
334,540,800 bytes of page-aligned load segments: 166,113,280 bytes of gaps per
loaded image. Codium has only 8,192 bytes of gaps, so this change mainly benefits
Chromium and its independently executed helpers.

## Validation

- All 22 hosted suites passed. The new suite compiles the actual ELF loader and
  mapping-range implementation with modeled physical frames/page tables. It
  covers sparse static/PIE images, reversed headers, shared boundary pages,
  BSS, malformed input, pre-existing mappings, allocation failure and mapping
  failure cleanup. Its sparse-image assertion fails against the old loader.
- The candidate kernel was built twice to refresh generated symbols. Its SHA256
  is `ed82eeae669656639e120121e10f15b22ebc892abbb4617fbab225136bec1e55`.
- Baseline and candidate VM disks came from the same clean seed with the same
  current Chromium/Codium wrappers and profiles: 8 CPUs, 4 GiB RAM, software
  rendering and audio disabled. Chromium rendered Google's consent page on
  both. Settled physical used memory was approximately 2,519,896,064 bytes
  before and 1,885,331,456 bytes after (about 605 MiB less). This is a sampled
  run comparison, not a startup-time benchmark or a peak-memory guarantee.
- Concurrent Codium startup still failed on both kernels. The baseline logged
  ELF allocation exhaustion. The candidate logged a stack allocation failure
  and a loader write fault on a COW mapping; Codium displayed a crashed-window
  dialog. Transient memory pressure is a working hypothesis, not a proven
  explanation for every fault. Post-failure free memory cannot establish the
  amount available at the instant of failure.

Baseline session: `7125e62d44d3464390d0ac3bd2594150` (checked shutdown and all
three offline filesystem checks passed). Candidate session:
`2513284b98a54ce5a91f8ec130cd6ec9` (investigation in progress at this checkpoint).
Local evidence is under `build/workloads/browser-memory-20261017/`, including
`hosted.log`, `elf-layouts.json`, baseline statistics and frozen boot artifacts;
VM serial logs and captures are under their bridge session directories.

## Next investigation at the sparse-loader checkpoint (`24f8a18`)

Measure allocation failures at the failure point and reduce duplicate exec
staging/image memory. Astra reviewed reusing eligible full staging pages with
managed COW references. That design is not implemented in this checkpoint:
`VmmMapPage` strips COW, so any implementation must use a suitable publication
API, preserve staging-image immutability, and test reference cleanup, aliases,
partial pages, overlaps and fork/write isolation. Browser/editor concurrent
edit/save/exit acceptance remains open.

## Follow-up: staging reuse and anonymous reservations

The dedicated `ReadExecutableImage` buffer now supplies eligible full initialized
pages directly to the main executable's user mappings. Each installed leaf owns
an independent reference and uses USER+COW through `VmmTryMapPage`. Partial
pages, BSS, noncongruent source offsets and every overlapping destination page
retain private zero-and-copy handling. The loader never writes a reused frame
through its physical alias. Releasing the staging mapping drops only its own
reference. Raw-buffer process APIs and their ABI retain copying; explicit owned
image entry points serve shell launch and execve. Interpreter loading still
copies. Physical descriptors retain Heap/KernelPid, so per-PID allocation
statistics do not fully attribute reused pages to applications.

Hosted loader coverage now includes read-only staging pages (an accidental
copy would fault on Linux), alias COW, both cleanup orders, single-owner write
promotion after staging release, invalid ownership-context shape and injected
mapping/allocation failures. All 22 hosted suites pass. Astra reviewed the
ownership and API changes. The native `sparse_exec_cow.S` probe checks actual
guest fork/write isolation for initialized pages and BSS, plus mprotect rejection
of the virtual gap. It passed directly and eight times through Bash exec on
the staging-reuse kernel, and again on the later kernel.

Staging reuse alone let concurrent Codium reach its editor, but the 4 GiB run
still exhausted memory. Bounded PMM failure diagnostics captured `free_pages=0`
at the fault, rather than inferring exhaustion from samples collected afterward.
Codium alone passed native editing, save/readback, close-tab/reopen and normal
window exit. The VM returned to ten tasks; heap integrity and all three offline
filesystem checks passed. Session `f924a5c2a1b144f1bbd3b7582df58ec6`, kernel
`93fc1dc43541632f361bb573c8cec06805f6e9cb343a2f2883c65bfcec0d985e`.

Private anonymous mmap previously allocated every accessible page immediately.
Ordinary mappings now reserve metadata and use the existing demand-zero fault
handler; explicit MAP_POPULATE retains eager allocation. Legacy file preload
also skips FileObject-backed vnodes: their faults use the inode's resident cache,
so filling the legacy cache only duplicates pages and pins vnodes unnecessarily.
No measured application-memory gain is attributed specifically to that guard.

`scripts/workloads/anonymous_memory.py` passed all eight semantic checks on
Linux. In otherwise idle 8-CPU, 4 GiB guest controls:

| Measurement | Before demand-zero | After demand-zero |
| --- | ---: | ---: |
| Physical cost of a 256 MiB reservation | 271,163,392 B | 98,304 B |
| Cost after touching four pages | 271,245,312 B | 204,800 B |
| 16 MiB MAP_POPULATE cost | 16,515,072 B | 15,912,960 B |
| Probe checks passing | 6/8 | 8/8 |

Samples use guest MemFree, include incidental runtime activity, and are bounded
by the otherwise idle test setup. The two old-kernel failures are the expected
physical-allocation assertions; kernel copyout, fork isolation, protection,
discard, explicit population and shared-anonymous semantics passed on both.
Control session `01e1a4170aa54169b279d93ed875d85c`; candidate session
`9b2106b3f85f4de9ba7e935a43cfdffa`. Both stopped with all three filesystem
checks passing. Candidate kernel:
`e71477f5c072e3914d3de3f37087f21d0c2364bd296288969dd0eb17aeb6a1f5`.

The later 4 GiB combined run saved and reopened the document, but still logged
allocation exhaustion and a Codium fault during window close. It is **not**
accepted as stable. The sampled headroom briefly settled near 61 MiB and later
fell below 30 MiB; neither sample establishes a safe bound.

## Concurrent acceptance with 8 GiB

With the same candidate kernel, 8 CPUs, software Wayland and audio disabled,
Chromium and Codium ran together as Brook. Native input dismissed Google's
consent prompt, submitted a search and navigated back. Codium edited, saved and
reopened `/data/home/codium menu acceptance.txt`; independent readback matched
all 151 bytes, owner 1000:100, SHA256
`326e5769361988651dad8bce05fd983f194ccce37bfcb9a2ff62d71816bcbe48`.
Both windows closed through their native controls without forced termination.
The guest returned to ten tasks, heap integrity passed, and serial contained no
PMM allocation failures, user faults or negative process exits. Checked shutdown
and all three offline filesystem checks passed. Session:
`7582fa88668049bf8a1c4a4e383cdbb8`.

`docs/images/brook_chromium_codium_1080.png` is an unmodified native 1920×1080
capture from that run. It shows a bounded browser/editor acceptance case, not
arbitrary extensions, long sessions or a solution to the 4 GiB limit.

Reboot session `9ea8c238505e4c8a9f81b7682fd4d710` independently confirmed the
same file hash, 151-byte length and 1000:100 ownership. Codium reopened the saved
contents through the standard wrapper, then exited through its native close
control. Ten tasks remained, heap integrity passed, no allocation failures or
user faults were logged, and checked shutdown plus all three offline filesystem
checks passed again. Local results and frozen artifacts are under
`build/workloads/browser-memory-20261017/`.

The next investigation targeted attribution by mapping kind. Independent exec images still duplicate
initialized pages across executions. Demand file-backed exec would need bounded
header reads, TLS preparation, file-gate publication and executable write
exclusion or immutable-version backing; the current file-map pin blocks truncate
but does not block ordinary writes. A bounded immutable content-dedup cache is
another option, subject to admission, lifetime and pressure-recovery testing.

## Rolling address-space census

The new `mem census` debug command, also available through the bridge's optional
`vm_stats(memory_details=true)`, distinguishes virtual reservations, RAM-backed
aliases and approximate proportional attribution by mapping kind. A weak
registry publishes only successfully constructed user address spaces; snapshot
references are acquired under its lock before final-unref can queue teardown.
The census uses an identity ceiling to terminate during continuous fork/exec.
It avoids following a raw process AS pointer across exec. It releases mapping
locks and AS pins before sending output. The sparse walker bounds actual table
visits per PT-lock acquisition and skips absent upper-level subtrees.

All 22 hosted suites pass. Actual VMM tests cover a 64 TiB sparse range, hidden
leaves, aliases, proportional accounting, unknown mapcounts, huge-leaf rejection
and a 5,000-leaf multi-batch walk. Registry tests cover unpublished construction,
duplicate/out-of-order publication, identity ceilings and reference-held
teardown. All 41 bridge tests pass, including complete census parsing and
opt-in/default statistics behavior. Astra reviewed the lifecycle and locking.

An 8-CPU, 8 GiB guest passed 300 native ELF fork/COW probes. Six complete censuses
ran while the latter 200 probes were actively launching/executing/exiting:
serial PASS counts advanced during each query. Afterward the registry contained
no user AS and the guest returned to nine kernel tasks. With Wayland and apps
running, representative snapshots were:

| Category | Chromium attribution | Chromium + Codium attribution |
| --- | ---: | ---: |
| Image | 1,346,973,693 B | 2,973,323,261 B |
| Anonymous | 114,475,005 B | 654,811,133 B |
| File | 24,170,489 B | 51,773,431 B |
| Memfd | 19,283,966 B | 32,112,637 B |
| Device RAM | 2,138,112 B | 4,059,136 B |
| Stack | 59,088,895 B | 126,402,559 B |
| Heap | 135,168 B | 135,168 B |
| Global physical used sample | 1,853,689,856 B | 4,191,510,528 B |

These are approximate rolling proportional-byte samples, not sums of RSS or a
simultaneous whole-system census. No unknown mapcounts were reported. The
combined image attribution is about 71% of the physical-used sample. Image
records show approximately 335 MB for each Chromium image and 203 MB for each
Codium image; descendants that forked without exec already share pages.
Anonymous virtual reservations totaled approximately 9.61 TB while their RAM
backing was approximately 658 MB, reinforcing the need for demand-zero mmap.

Both apps exited through native window controls. A census series straddled
Chromium close: completed-AS counts fell from ten to one (Wayland), and physical
used memory fell from approximately 1.88 GB to 290 MB. Ten tasks remained and
heap integrity passed. The serial log has no allocation failures, user faults,
negative process exits or map-accounting violations. Checked shutdown and all
three offline filesystem checks passed. Session:
`525dcb84aa9c4aca855d494ab2e8a67c`; kernel:
`211ab4a124948270039c9ab2597e2c923c2b3ff9ed817fc31ad999f069a84ef6`.
Local evidence: `build/workloads/memory-census-20261017/`, notably `stress.json`,
`chromium-settled.json`, `combined-later.json`, `chromium-close-census.json` and
`after-close.json`. Census sampling affects execution and is not a startup-time
benchmark. It misses in-progress executable staging, among other memory listed
in the bridge documentation.

The next optimisation should share byte-identical initialized ELF pages across
independent execs. A bounded immutable content cache can compare complete pages
after hashing, publish only COW aliases and release idle cache references after
AS cleanup. It must retain references through lookup/publication, unlink before
eviction, avoid PMM-lock inversion or quarantine waits, and preserve raw-buffer,
partial-page, BSS and overlap behavior. Cache admission must avoid sequential
scan thrashing; existing fork sharing must not be counted as additional savings.
This cache is not implemented by the census checkpoint.

## Immutable executable-page cache follow-up

A fixed content cache now shares eligible full initialized ELF pages across
independent launches. It owns a reference to each canonical frame; each user
mapping owns a separate reference and remains COW. Hashing happens outside the
cache lock, and a complete 4096-byte comparison is required before reuse.
The loader skips direct-map copying into full reused COW pages even when the
canonical frame differs from the staging source. Raw-buffer loaders, interpreter
loading, partial pages, BSS and overlapping PT_LOAD pages retain their existing
copy/zero behavior.

The table uses 262,144 slots (4 MiB), permits 131,072 canonical frames (512 MiB)
and probes at most 32 slots. It scans beyond holes before admission. There is
no active-entry eviction during sequential image scans; capacity/probe refusal
returns the uncached owned frame successfully. Trimming scans at most 128 slots
and unlinks at most 32 entries per IRQ-lock hold, releasing frame references
outside the cache lock. A synchronized PMM sole-reference snapshot gates idle
reclamation. Cleanup runs after AS destruction and after dedicated image
release, covering either failed-construction cleanup order. No filesystem
operation, allocation or quarantine wait runs under the cache lock. Astra's
read-only review found no blocking lifetime/lock-order issue.

All **23 hosted suites pass**, including the actual cache implementation under
8 concurrent adopters and a trimmer (8,000 equal candidates), forced unequal
hash collisions, probe/capacity refusal, holes before a canonical match, same
frame adoption, cache clearing with live users and both external-release orders.
The actual ELF loader test protects staging memory read-only and now aliases
equal bytes from different source physical pages; the canonical-copy shortcut
must preserve that protection. Existing partial/BSS/overlap, mapping/allocation
failure and raw-buffer cases remain covered. Two-pass release kernel/module
build passed.

### 4 GiB workload acceptance

Session `095fbc0782cb43629905803e5710dcaa`, 8 CPUs, **4 GiB**, software Wayland,
audio disabled. Kernel SHA-256:
`3af99054fd37878cd881fec57197511857a50bb8c00dc4420afcdd4e9f9480dd`.
Disks were cloned from the clean census checkpoint; profiles were already
populated. Chromium loaded Google, accepted a search and returned via native
Back. Codium edited, saved, closed its tab and reopened the file while Chromium
remained running. Both applications then exited through native window controls.

Representative rolling samples:

| Measurement | Previous census run (8 GiB) | Cache run (4 GiB) |
|---|---:|---:|
| Global physical used | 4,191,510,528 B | 1,921,335,296 B |
| Image proportional bytes | 2,973,323,261 B | 646,025,200 B |
| Anonymous proportional bytes | 654,811,133 B | 706,940,928 B |
| Stack proportional bytes | 126,402,559 B | 126,541,822 B |

These are representative workload snapshots, **not** matched controlled timing
or peak-memory benchmarks. The census is rolling; stats and attribution are
queried sequentially, and constructions/staging/quarantine are excluded. The
cache run's earlier combined sample reported 1,933,066,240 physical used and
645,238,769 image proportional bytes. The later sample contained 20 address
spaces. Cache hits include existing same-frame aliases and must not be turned
into a saving figure. There were 1,003 successful uncached fallbacks as the
bounded cache filled during transient load; no PMM allocation-failure reports
were observed. Startups remain slow.

After native exits: 10 tasks (desktop plus kernel services), one user AS,
276,959,232 physical bytes used and 14 desktop cache entries. Another **100
native fork/COW probes passed**, then 10 tasks, 277,082,112 bytes used and the
same 14 entries remained. Heap integrity was OK. Checked guest sync/stop and
all three offline filesystem checks returned zero. One debug-channel read timed
out during Codium shutdown; a later read confirmed its complete exit and normal
browser operation. A scheduler `Ready`/not-enqueued diagnostic was also observed
for a Codium task during the live run; it did not prevent the tested save/reopen
or native exits and remains a follow-up diagnostic, not a clean bill of health
for all scheduling races.

Saved `/data/home/codium menu acceptance.txt`: 192 bytes, UID/GID `1000:100`,
SHA-256 `25f84de257c1aad6b7a6ee5ef349a0209c8c872296e20b5e7ab5c4d49ede8795`.
Unmodified 1920×1080 screenshot:
`docs/images/brook_chromium_codium_4g_1080.png`, source artifact
`e2585ed22caa4b94b04784610ed12a88.png`.

Local evidence: `build/workloads/exec-cache-20261017/` (`metadata.json`,
`combined-startup.json`, `combined-later.json`, `combined-edited.json`,
`codium-closed.json`, `apps-closed.json`, `stress-final.json`,
`filesystem-results.json`, `acceptance-serial.log`). Hosted/build logs:
`build/exec-cache-host-tests.log`, `build/exec-cache-kernel-build.log`.


Reboot session `468ac11ea0cf4dc0ac28d01158eb254c` used the same kernel and 4 GiB
disks. Independent guest readback preserved the 192-byte file, `1000:100`
ownership and the SHA-256 above. Codium restored and displayed all six saved
lines in its native window (artifact `cc222ef4e4cf4c2f86401d6b6a4544b8.png`).


The reboot's **second native exit did not fully clean up**: four blocked threads
(TIDs 37, 52, 53, 88; TGID 143) and a terminated/reapable leader 143 remained.
The window's original process had exited; PID 143 was forked by TID 36 during
shutdown, then exec'd Codium. `/proc/cmdline` is synthetic and does not establish
its helper role. A non-destructive Ctrl+F12 dump showed an identity-matched
leader with `asLiveThreads=4`, syscall 202, refcount zero, and live threads
blocked in futex/epoll. Two ASes remained, including about 201 MB of image
proportional bytes for group 143; physical use was 478,638,080 B and the cache
held 47,066 entries. Those entries were correctly retained for live users.
This is an outstanding app-exit regression, **not repeated-exit acceptance**.
The immediate default-fatal-signal path currently marks a single task terminated,
unlike deferred fatal delivery's group cancellation; this is a source-backed
hypothesis for follow-up, not a proven cause. Preserve
`reboot-closed.json`, `reboot-closed-later.json` and the reboot serial hang dump.
An explicit guest SIGKILL to a surviving group member was requested for cleanup.

The explicit SIGKILL cleaned the complete helper group: 10 tasks, 243,580,928
physical bytes used, 14 desktop cache entries and heap integrity OK. Checked
shutdown and the reboot's three offline filesystem checks all returned zero.
This forced cleanup is recorded separately from native app-exit acceptance.
Astra confirmed that synchronous group-kill cannot be called from every signal
sender: timer/terminal callers may hold IRQ/terminal locks. Follow-up needs a
non-waiting fatal-group request with proper snapshot references, birth latch,
masked-signal behavior and cooperative waiter cancellation.

## Fatal-signal group cancellation follow-up

The focused `fatalgroupcheck` control reproduces the cleanup defect independently
of Electron. With identical static probe bytes, Linux passes all twelve cases;
the cache checkpoint kernel fails nine (term to leader/thread, tgkill KILL to a
thread, alarm-group, masked TERM, clone race, USR1, masked USR2 and real-time 34).
Handled/ignored TERM and raw leader SYS_exit controls pass. The corrected alarm
case uses raw SYS_alarm; an earlier exploratory run used musl's unimplemented
setitimer wrapper and is not the final baseline. Baseline session
`f0529f00acaf492cabea9f9e484f3dd3`, kernel
`3af99054fd37878cd881fec57197511857a50bb8c00dc4420afcdd4e9f9480dd`.

`SchedulerRequestFatalSignal` now snapshots every generation-matched group
member under the registry lock and publishes the thread-birth exit latch.
It marks members outside that lock, releases notification-safe references with
IRQs still disabled, and never waits for peer quiescence. Existing cooperative
operation/wait cancellation and reaper gates drain the group. Timer and terminal
senders therefore avoid the synchronous syscall group-kill helper's waits and
IRQ enabling. Both SIGKILL and unblocked catchable default-fatal delivery use
this path, including tgkill to a nonleader. Masked catchable signals remain
pending; the existing wake is preserved for synchronous signal waits. Deferred
and immediate default action classification now agree, including user and
real-time fatal signals. Raw SYS_exit continues to terminate only its caller.
Astra's read-only review found no blocking lock/reference issue.

Candidate kernel
`fc6e8cf96cb494a83d316d1f73569fd3fdf0d39f2c1e55f0bbe2d2cb41ed31c1`,
session `60b27dc31cb7485fb667114d670e3e73`, 8 CPUs / 4 GiB:
**48/48 cases pass** across four rounds, followed by 9 kernel tasks, zero cache
entries and heap integrity OK. Checked shutdown and all three offline filesystem
checks passed. All 23 hosted suites and the two-pass release build passed.

The same candidate rebooted as `7346cfc4fe2c4d73837b0f4f6ea0d93e` and passed
16 filesystem-cancellation rounds with 8 workers on ext2, another 16 on FAT,
32 mixed futex/pipe/sleep exit_group rounds, and the existing wait-mask suite
with zero failures. All test tasks exited, cache entries returned to zero and
heap integrity stayed OK. These checks exercise cancellation during filesystem
activity; they do not prove all process-wide signal routing or stop/continue
semantics.

Local evidence: `build/workloads/fatal-signals-20261017/`, especially
`metadata.json`, `linux-control-final.log`, `candidate-build-tests.log`,
`final-kernel-build.log`, `baseline/guest-final-serial.log`,
`candidate-probe-serial.log`, `candidate-probe-results.json`,
`controls-results.json` and the corresponding session serial logs. The probe
source and invocation are documented in `tools/at-syscall-probe/README.md`.

The raw leader-exit control checks only that peers survive and the eventual
process result is a normal exit. Linux reports the final worker group's zero
status, while Brook preserves the leader's earlier status 23. That wait-status
difference is a separate follow-up; it is not covered by this fatal-signal fix.


The first candidate Codium-only native close returned to 10 tasks, 14 desktop
cache entries, 243,896,320 physical bytes used and heap integrity OK
(`codium-close-1.json`). A short startup CPU/CS capture was preserved at
`build/vm-bridge/7346cfc4fe2c4d73837b0f4f6ea0d93e/artifacts/090eb79af994487981c031b67f5c14e4.speedscope.json`
(2,245 CPU samples, 52,438 switches). It is a late, instrumented startup slice,
not a before/after performance test. MappingRangeFind and the page-fault path
are visible hot spots among busy samples; startup optimisation remains open.


### Repeated native close and reboot acceptance

The second candidate Codium cycle ran alongside Chromium, edited/saved/reopened
the persistent document and closed through its native window control. No Codium
tasks remained (`codium-close-2.json`); Chromium remained live. Native Chromium
closure then returned to 10 tasks, 14 desktop cache entries, 278,663,168 physical
bytes used and heap integrity OK (`apps-closed.json`). Checked shutdown and all
three offline filesystem checks passed.

Reboot session `13b736cafa48412688323df1cbd6b74a` verified both filesystem
fixtures (`FSEXITCHECK VERIFY PASS bytes=131119` twice). The updated document
persisted at 224 bytes with owner `1000:100`, SHA-256
`561e011c75eb459481db8a0779e57aac56ca24593c3afa30a66c2168caeaca37`.
Codium displayed all seven saved lines, then closed natively for a third time.
The early close sample still observed ordinary asynchronous shutdown; the later
sample confirmed 10 tasks, 14 desktop cache entries, 243,560,448 physical bytes
used and heap integrity OK (`codium-close-3-reboot-later.json`). No forced kill
was used in any of these three candidate native-close cycles. Checked shutdown
and all three final offline filesystem checks returned zero. These bounded
runs accept the observed helper cleanup regression, not every possible future
exit race. Startup remains slow. The original failure evidence is preserved
above rather than relabelled as successful acceptance.


The final probe strengthens the clone-race setup by waiting for a successful
thread birth before publishing readiness to its parent. Linux still passes all
12 cases. The unchanged baseline kernel rerun
`378e96afa7824247b75329c9f88e14fc` still fails the same nine cases, with cleanup
and all three filesystem checks passing. Final probe SHA-256:
`0860b9149e2c3e5d7d08769b65712f8798c92488873c1a46b80b8a44f2750ad4`.
The metadata preserves the earlier probe hash separately.

The matching final candidate session `d7cd0fd4c80e41b99aa1cef5c2fb330c` passes
all 48 strengthened cases, returning to 9 kernel tasks and zero cache entries
with heap integrity OK. Checked shutdown and three offline filesystem checks
all pass (`strengthened-final-results.json`, `strengthened-filesystem-results.json`).


### Remote checkpoint: launch-feedback groundwork (unfinished)

The next work item is launch feedback and display scaling. This checkpoint
adds a bounded, value-only launch-feedback state machine, a hosted suite for
identity matching, duplicate requests, stale completions, slot exhaustion and
timeouts, plus a process launch token and scheduler liveness query. These
foundations are not wired into the shell, launcher, compositor or taskbar yet;
there is no new visible launch indicator in this checkpoint.

Remaining integration includes moving executable loading off the compositor
thread, serializing shell dispatch, propagating launch tokens before process
publication, matching native and Wayland windows, and rendering pending and
failed launches. Process options must be installed before scheduler publication
to avoid accessing an already-exited child. Display scaling, App Gallery, VM
recording and the remaining workload checks stay open in the active goal.

The previously accepted VM kernel and clean disks remain under
`build/workloads/fatal-signals-20261017/`; this groundwork has not been booted
for VM acceptance. No copyrighted game data or runtime disk images are included
in the checkpoint.

Validation for this groundwork checkpoint: all 24 hosted suites pass, including
the new state-machine suite, and the two-pass release kernel build passes with
all 12 driver modules resolving their imports.
