# Mapped file growth checkpoint

CEF's real startup capture attempts ftruncate64 from 8,192 to 270,336 bytes
while retaining a mapping. Brook previously rejected every resize of a mapped
vnode with EBUSY, including growth. This checkpoint enables the observed growth
operation without dropping canonical pages or invalidating borrowed PTEs.

Ext2 and tmpfs explicitly opt their canonical file objects into a strict-growth
callback. The callback contract preserves all bytes below page-aligned old EOF,
even when an error is returned, and never shrinks the backing. Other backends
remain rejected. The file-object path re-stats EOF under its inode gate so a
concurrent ordinary write cannot turn a stale growth request into shrink. It
retains all canonical frames, exposure leases and aliases, propagates errors,
and advances the generation even if a failed attempt committed a larger EOF.
The existing VFS mutation gate still excludes first-pin publication.

Mapped shrink, same-size truncate and growth from partial-page old EOF remain
rejected. Those cases are not a general mapped-resize implementation. No public
module layout changes; ABI remains 50. Memfd behavior is unchanged.

## Verification

Production ELF SHA256:
`3812b40f33d9cb3a08eb4edf60982b07e4d6c06deecd8c9c19e42114c2ace751`.

The actual file-object host fixture checks existing frame identity, shared
stores, retained writeback, zero-filled new pages, disabled backends, invalid
resize requests, current-size revalidation, backend errors before growth and
errors after a committed growth. File-object and tmpfs ASan/UBSan fixtures pass.
The full assertion-enabled host suite passes 101 of 102 in its first run; the
unrelated audio-delivery fixture times out during parallel kernel compilation
and passes in isolation afterward. Both results are retained. Production
kernel, drivers and standalone tests build; exact-ELF entry-state and scheduler
mutation audits pass.

`scripts/workloads/mapped_file_growth_check.py` passes on Linux and through real
Brook syscalls on tmpfs, the data filesystem and the runtime disk. The parent
and child keep the old mappings alive during growth, verify old mapped stores
through independent readonly aliases and pread, check zero-filled new pages,
write the new region in the child, then verify fsync and reopened contents.
All native jobs end with empty groups and both streams at EOF. Native Unix IPC,
DRM namespace binds, tmpfs quotas and PulseAudio write fallback also pass.

## Steam result

The first unmodified-helper run still exits 135 before login. Its footprint
reaches ten 25 MiB plus 16-byte pools and the newly appended CEF log contains
two persistent-memory corruption warnings. Thus safe aligned growth fixes a
demonstrated compatibility gap, but does not establish the cause of the pool
buildup or corruption. The next comparison uses a fresh private profile and a
delegating probe that records successful CEF-sized growth as well as failures.
The original failed client and monitor drain completely; PulseAudio and
Xwayland exit normally. Heap integrity, checked shutdown, final serial checks,
the original owned launcher wait and all four offline filesystem checks pass.

Private evidence:
`build/reviews/steam-mapped-growth-off-gpu-1ms-fixed-mlfq-a-20261023` and
`build/workloads/steam-mapped-growth-20261023/`.

## Partial-page EOF follow-up

The fresh-profile capture exposes another real growth case: CEF requests
2,048 → 57,344 bytes, 55,296 → 106,496 bytes, and 75,776 → 124,928 bytes.
The initial aligned-only implementation rejects these. The capture has no new
persistent-memory corruption warning but still ends in shared-memory pressure;
clearing the cache has not established login.

The follow-up admits strict growth from partial-page EOF as well. After growth,
it zeros the entire old canonical page tail, including bytes beyond new EOF
when growth ends within the same page. Existing shared aliases observe those
zeros; independent private CoW frames retain their contents. No old frame is
freed or detached. After a backend error, EOF is re-statted: unchanged EOF keeps
the old tail, committed growth clears it, and unknown EOF conservatively clears
only the invalid tail while retaining every valid prefix byte. Same-size
truncate and shrink remain rejected.

The Linux control confirms both tail behaviors. The new guest also passes them
on tmpfs, the data disk and the runtime disk, together with the earlier fork,
alias and fsync cases. All 102 assertion-enabled host tests pass in one run;
file-object and tmpfs ASan/UBSan fixtures pass. Exact-ELF entry-state and scheduler
mutation audits pass for ABI 50 ELF
`1f28dfd3460888052665662bd15399957b82065854d36265f38c323180aee04d`.
The renewed native IPC, DRM, quota and Pulse controls also pass.

The fresh-profile application capture confirms that the formerly failing
2,048 → 57,344-byte request and subsequent requests to 106,496 and 124,928 bytes
now return success. The captured CEF resize calls have no errors. Steam still
creates ten 25 MiB plus 16-byte pools and exits 135 before login. Its newly
appended log contains one persistent-memory corruption warning and a
`ChildThreadImpl::EnsureConnected()` entry. These remaining symptoms have not
been attributed to the growth failures. Thread-level pool attribution is the
next diagnostic distinction: repeated caller stacks alone do not prove that
one thread is retrying rather than multiple threads creating connections.

All original jobs drain, the pristine helper wrapper is restored, and both
services exit normally. Heap, checked shutdown, final serial acceptance,
original launcher wait and all four filesystem checks pass in
`build/reviews/steam-mapped-growth-off-gpu-1ms-fixed-mlfq-c-20261023`.

A controlled call to the installed library's public `cef_version_info` API
reports CEF 126, commit number 3171, Chromium 126.0.6478.183. This verifies the
version used for the source comparisons; no client data is exported.


## 16 GiB / unrestricted byte-quota experiment

The opt-in QEMU setting `-fw_cfg name=opt/tmpfs-unbounded,string=1`
sets the aggregate tmpfs and canonical `/dev/shm` byte ceilings to the largest
page-aligned signed offset. Pages remain lazily allocated and subject to real
physical-memory availability; inode/mount limits and explicit smaller mount
quotas remain enforced. The default RAM-derived ceilings are unchanged.

Review `steam-mapped-growth-off-gpu-1ms-fixed-mlfq-d-20261023` uses 16 GiB,
eight CPUs and the same MLFQ/Intel Venus/1 ms host polling configuration.
The actual guest reports 16,724,896 KiB RAM and the requested byte ceiling.
Native IPC, DRM, explicit quota, Pulse and all three mapped-growth filesystem
controls pass, including the partial-page alias/CoW checks. Production entry
and scheduler mutation audits pass on frozen ELF
`b5e5cbfa92bc711f67d040a9346534f969b1f655da3873fe0027bfda2448f52b`.

Unlike the earlier capped runs, Steam remains alive while its shared-memory
pool count continues increasing. The monitor exceeds 1 GiB of logical files
and 2 GiB of charged backing/cache memory with abundant physical RAM still
free. No login appears. The bounded capture still reports the persistent-memory
corruption warning and multiple `ChildThreadImpl::EnsureConnected()` messages.
The new probe adds thread IDs: one parent records 52 pool creations, with
12 on one worker and 40 on its main thread; captured allocation calls have no
unexpected errors. Thus the old quota was a real constraint but removing it
does not resolve startup, and repeated same-thread allocation is now observed.
This supports investigating failed IPC connections/recovery rather than
assuming the pool count merely reflects distinct workers. It does not prove
the exact retry cause or an infinite leak.

The experiment is stopped deliberately after continued growth, rather than
waiting for physical exhaustion. Exit 137 is the agent's forced stop following
an unsuccessful graceful stop, not a spontaneous crash or an OOM result.
Proprietary payloads and raw logs remain in private guest evidence.

Final monitor totals: 302 samples, 56 pools, 1,473,993,264 peak logical
bytes and 2,973,478,912 peak charged bytes. The minimum recorded free physical
memory is 12,690,672 KiB. The last sample follows the deliberate application
stop and must not be interpreted as a spontaneous plateau. All original job
groups drain with stdout/stderr EOF. The pristine wrapper is restored, both
services exit normally, heap integrity passes, the original VM owner exits 0,
and final serial acceptance plus all four offline filesystem checks pass.


## Follow-up controls: uninstrumented startup and IPC metadata

The first bounded socket probe records successful descriptor transfers and no
ancillary truncation in its captured calls. Missing DBus endpoints account for
many connect errors; IPv4 connection refusals also appear in steamclient, but
the destination and relevance are not established. X11 consumed much of its
per-process successful-call budget, so this is not full IPC coverage.
A revised probe samples successes per descriptor. Its direct CEF calls show
bidirectional socket traffic and rights transfers before connection timeouts;
there are no captured unexpected sendmsg/recvmsg errors or MSG_CTRUNC events.
No peer-credential checks were observed, which is not proof that all credential
paths are correct.

The fresh-boot, uninstrumented first phase of review
`steam-mapped-growth-off-gpu-1ms-fixed-mlfq-f-20261023` uses the same frozen
kernel, 16 GiB and unrestricted byte quotas. The wrapper hash remains pristine,
and the expected probe file is absent. The attempted maps audit does not
prove library absence: Brook currently reports only heap/stack approximations
in `/proc/<pid>/maps`, as the later progress-probe control establishes. It still produces the corruption warning, repeated connection
timeouts and continued pool growth without a login screen. Before the stop
request it has 40 pools and approximately 1.05 billion logical shared-memory
bytes; its full 181-sample capture including the deliberate stop reaches
56 pools, 1,473,993,264 logical bytes and 2,977,292,288 charged bytes. The
instrumentation is therefore not the sole cause of this failure.

A separate fresh-profile probe phase in the same F VM has a distinct baseline
and retains the first phase's shared files. Its footprint must not be treated
as a clean-boot or performance comparison. It localizes later CEF exchanges but
does not identify the connection failure's cause. Both phases' original jobs
drain, the original wrapper is restored, services exit normally, heap integrity
passes, the original VM owner exits 0, and final serial acceptance and all four
offline filesystem checks pass. The preceding E socket-capture VM has the same
checked-stop receipts. No new kernel change follows from these probes yet.


The G feature-off run remains uninstrumented and does not reach login. Its
wrapper explicitly appends `--disable-features=MojoIpcz,MojoLinuxChannelSharedMem`
while preserving the existing disabled-feature list. However, the live argv
audit fails because current procfs returns only `Process::name` for cmdline,
not the argv vector. Therefore this run does not independently prove which
features CEF selected, and is not accepted as ruling out shared-memory IPC.
The original wrapper is restored; original jobs/groups/EOF, services, heap,
serial acceptance, original owner and four offline filesystem checks pass.

## Verified feature switches after live cmdline support

Review `steam-mapped-growth-off-gpu-1ms-fixed-mlfq-j-20261023` repeats the
feature-off control on the live-cmdline kernel, with a fresh boot/profile,
16 GiB RAM, unrestricted byte quotas, and no application preload. The live
browser argument audit passes, and the lifetime capture also observes a utility
child carrying both disabled-feature tokens. This verifies argument delivery;
it does not independently inspect the selected transport implementation.

Steam still shows a black Xwayland surface instead of login. Across 181 shared
memory samples, peak logical bytes reach 1,365,263,200, charged bytes reach
2,755,887,104, and 52 files have the recurring 26,214,416-byte pool size.
Two child connection timeout messages and two network-service restart locations
are counted. The persistent-allocator corruption warning is absent from this
run, so it is not a necessary accompanying symptom of the login failure.

The agent stops Steam after observation; exit 137 is deliberate, not an OOM or
spontaneous crash. Original groups drain, the wrapper is restored, services exit
normally, heap integrity passes, and completed metadata sync precedes QMP quit.
The original VM owner exits zero; serial acceptance and all four offline
filesystem checks pass. An earlier attempt, I, exited before guest boot because
SDL selected an unavailable host display backend and contributes no Steam data.

Astra's independent review identifies two remaining discriminators: a verified
longer child connection watchdog, then matched Linux/Brook readiness and socket
backpressure metadata. Existing probes omit `send()` and readiness calls,
suppress EAGAIN/EINPROGRESS, and share limited success budgets across numeric
FD reuse. Their healthy early exchanges cannot establish complete channel
progress. The source-visible epoll open-file-identity gap remains a hypothesis
until relevant Steam descriptor choreography is observed.

The next fresh-boot control, K, retains default Mojo feature arguments and adds
only `--ipc-connection-timeout=60`. The complete token is verified in both the
browser and a live utility child, with no application preload. This child keeps
the same PID/incarnation and is observed alive from guest monotonic 112.839 s to
365.127 s, yet no `EnsureConnected()` message appears. Steam still has a black
surface and no login. Therefore this is not evidence of a completed handshake,
nor a confirmed change from a 15-second to 60-second executed callback: the
vendor behavior, watchdog installation and event-loop progress remain unproven.

The original 181-sample memory capture reaches 55 recurring pools,
1,447,787,056 logical bytes and 2,919,608,320 charged bytes. One allocator warning
appears. Pool growth thus occurs before a logged connection watchdog or observed
utility restart; repeated utility restarts alone do not explain it. After the
bounded deadline observation, a distinct five-second built-in epoll-counter
phase captures activity in browser tasks. Its counters only cover calls started
while enabled and cannot exclude older blocked waits in other tasks. The next
useful comparison needs socket backpressure/readiness metadata on both Linux
and Brook, without interpreting absent probe samples as absent operations.

K cleanup initially finds eight live descendants after the Steam leader exits
zero on the agent's stop request. The remaining utility is identified by its
procfs incarnation and executable, then exits on a targeted SIGTERM; no SIGKILL
is needed for that utility. The original job subsequently reports zero live
descendants and both EOF receipts. The wrapper is restored, services exit
normally, heap integrity and metadata sync pass, the original VM owner exits
zero, and final serial acceptance plus all four offline filesystem checks pass.

## Probed Linux control at matching memory capacity

An isolated Linux reference uses 16 GiB and eight vCPUs, the same staged Steam
version and the v7 probe source with only its output path changed. A private
root-image copy, guest-only display and loopback key-authenticated SSH keep this
separate from the host desktop. The instrumented client reaches a visible QR
login screen. This is Linux evidence, not a Brook login result.

Its captured shared-memory footprint is 58,414,784 logical bytes, with only two
26,214,416-byte pools. The complete captured CEF log has zero allocator warnings
and zero child connection timeouts. The metadata capture contains 1,112 events,
826 IPC events, no unexpected captured IPC errors, no ancillary truncation and
no observed peer-credential check. Thirteen direct CEF operation/flag/control
shapes overlap the failing Brook F capture; Linux adds a successful sendmsg with
24 control bytes. That difference is not a demonstrated cause: call sampling is
bounded, readiness and EAGAIN remain omitted, and F's second phase retains old
shared files. Distribution/runtime services also differ. No payload-level
correctness or syscall equivalence is inferred from these overlapping shapes.

The owned transient Steam service stops successfully with its cgroup, its
wrapper is restored, guest sync completes, and QMP requests system powerdown.
The original Linux QEMU owner exits zero without reaching its timeout.
The Linux serial log confirms powerdown, and the read-only offline filesystem
check exits zero; the reference acceptance report is accepted.

## Progress and readiness probe

A new private probe records scalar/vector socket byte counts, sampled EAGAIN,
readiness masks, opaque epoll data tokens and interposed close/dup/fork
lifecycles. Sampling budgets distinguish operation and result class. It records
the first sixteen calls and later powers of two per descriptor/operation/class,
with a 16,384-row process cap and an explicit overflow marker. It preserves
returned errno and uses raw syscalls for its own log output. Its own-process
smoke test passes both unloaded and preloaded, covering payload/errno retention,
backpressure, an epoll watch surviving a closed descriptor with a live alias,
numeric descriptor reuse, eventfd and fork.

The fresh 16-GiB/eight-vCPU Linux control reaches visible QR login with this
probe. Its captured metadata has 9,405 rows, no overflow and two recurring large
pools. In Brook review L, the checked offline file has 18,004 schema-valid rows
with no overflow; the browser PID and child PIDs emit their own probe events.
The 181-sample memory monitor reaches 52 large pools and 1,369,143,808 logical
bytes. The longer complete trace records 86 exact-size allocations from the
same browser PID. The bounded monitor and longer trace are different windows.
Seven child connection timeout messages and one allocator warning are counted.
Steam remains a black surface without login.

The first live full-trace export loses bridge output and is explicitly rejected.
Its original job is drained without replay. After completed guest sync, normal
original-owner exit, serial acceptance and four passing offline filesystem
checks, only the probe's exact metadata file is recovered read-only from the
private data image. Its full schema validates. The Linux service/cgroup is also
stopped, its wrapper restored, guest sync and normal powerdown complete, and its
read-only offline filesystem check passes. Raw application logs and payloads
remain private.

The maps presence audit fails because `GenPidMaps` currently synthesizes heap
and stack only; this does not establish probe absence. Earlier map-only absence
claims must not be used as evidence. The corrected uninstrumented control still
has independent fresh-boot, pristine-wrapper and expected-probe-file-absence
observations. Actual progress events prove interception for their emitting PIDs
and calls, not every possible syscall path.

A stronger difference is the shared-memory handoff: on Linux a second PID maps
both 26,214,416-byte pools successfully with MAP_SHARED and read/write access.
In Brook's capture only the creating webhelper PID maps this size. A recovered
older allocation-stack sample has immediate `steamwebhelper` frames; repeated
pairs share an allocator prefix before worker-thread or libcef callback paths.
These pools must not be called Mojo pools without further attribution.

Successful-only utility-channel receives also occur on Linux, so the absence of
sampled EAGAIN on one Brook utility descriptor is not evidence that MSG_DONTWAIT
is ignored. Source inspection confirms that flag reaches the Unix read helper.
The simple completed-call watch/alias analysis found no demonstrated matching
epoll-lifetime failure. Different OUT-event and TCP-progress counts could be
downstream of missing connection setup. Neither observation justifies an epoll
repair yet.

The next bounded probe adds socket/bind/listen/accept and endpoint-equivalence
metadata, plus only a few relative allocation stacks per participating thread.
It emits no raw addresses, socket paths, SCM_RIGHTS values or payload bytes.
The purpose is to identify the missing peer handoff and listener lifecycle
before selecting a kernel change. Interposition remains partial: hidden libc
calls/direct syscalls and unwrapped fcntl duplication can bypass it, and cached
FD classifications can outlive such hidden closes. Negative samples and
interposed generations therefore do not prove full syscall coverage.

## Object acquisition control (2026-10-24)

Astra's review identifies an important ambiguity in the preceding capture:
missing interposed `recvmsg` does not establish a hidden descriptor receive.
The parent might reopen named objects, and identical mapping sizes alone do not
prove a common backing object. Pathname equality likewise does not establish
that separate filesystem contexts resolve the same vnode.

The fresh focused Linux control confirms **named reopening**. Its helper
truncates and maps two 26,214,416-byte objects. The Steam parent subsequently
calls `shm_open` twice; each returned descriptor has the same device/inode/size
as one of the helper's objects, and both parent mappings succeed. It reaches a
visually verified QR login screen with this probe. No login is authenticated.
The bounded trace contains 112 numeric/opaque-token metadata rows, compared
with thousands in the preceding readiness capture. The owned service stops,
the wrapper is restored, sync and normal powerdown complete, the original
owner exits zero, and read-only offline filesystem acceptance passes.

The probe's synthetic control passes both unloaded and preloaded. It verifies
named reopening and SCM_RIGHTS acquisition of the same object across processes,
shared data integrity, socket-path identity, failed-connect ENOENT, failed
truncate EBADF, and preserved result/errno behavior. Literal paths, payloads,
raw pointers and received descriptor values are not exported. It is still
interposition, not complete syscall coverage; relative open paths and hidden
libc/direct syscall operations can bypass name classification. A 1,024-row
per-process limit bounds logging, and the capture must be checked for exhaustion.

Brook endpoint review M remains a failed login: the completed frame is black,
and the checked offline trace contains 134 helper pool allocations with no
observed matching-size parent mapping. It contains 22,202 validated IPC/stack
rows, including a parent overflow marker at approximately 380 guest seconds.
Earlier observations remain bounded by that marker. Failed pathname connects
are present, but the old endpoint probe omits tokens for ENOENT/EACCES, so they
cannot establish the intended endpoint. A later kernel capture also fails its
identity precondition when two Steam leaders exist; it does not enable tracing.
The earlier zero-row kernel capture selects one task, excludes sibling threads
and several relevant operations, and cannot rule out a handoff.

M's stopped-image extraction follows checked sync, original-owner exit zero,
heap integrity, serial acceptance and four passing filesystem checks. Its
Steam workload required agent termination and is not a successful application
exit or login result. The next Brook control uses the focused object probe to
resolve whether the named opens are absent, rejected, or resolve differently.
Private sources/receipts are in `build/workloads/steam-object-handoff-20261024/`
and review M; application payloads, profiles, logs and login images stay private.

The focused Brook review N also fails visually: its Steam surface remains black.
At the first repeat (111.727 guest seconds), 102 metadata rows identify four
helper-created pools and only helper mappings. The stop request follows this
observation; the final checked offline file has 297 validated rows and 26
helper pool mappings, with no observed parent opens or mappings of those exact
objects. Neither participant reaches the 1,024-row cap (maximum sequences 174
and 116). Pool truncate logging intentionally stops after eight calls, so the
final mapping count must not be described as an unbounded allocation count.

All thirteen captured failed pathname connects share the locally computed
opaque token for the standard system D-Bus endpoint. None matches either
helper bind token. They do not demonstrate a failed Steam control connection.
The bind probe does not establish successful listening; that remains to check.
There is also positive evidence against a general shared-memory namespace
split: parent and helper successfully open fifteen common name tokens, and
matching device/inode identities establish shared backing for small objects.
For example, the parent creates an object later opened by the helper at size
8,208 bytes; this occurs repeatedly. The helper opens twelve such small objects
in N versus one in the successful Linux control. The probe does not capture
8,208-byte mappings, their contents, or their synchronization, so it cannot yet
establish correct control-region coherence or explain the retries.

The next discriminator is a bounded check of successful listening and the
parent's view of the specific helper socket and pool objects, together with
mapping metadata for these small common control objects. Generic failed
connects, additional RAM and broad epoll changes are not supported next fixes.
Interposed absence remains weaker than complete syscall coverage. N's wrapper
is restored, all tracked groups drain, heap and checked sync pass, original
QEMU exits zero, and serial plus four offline filesystem checks pass. Its
application exit 137 follows agent termination, not successful client shutdown.
