# Orderly desktop shutdown and restart

The launcher now provides **Shut down**, **Restart** and **Cancel**. Shell
`shutdown`/`halt`/`reboot` and MCP `vm_lifecycle` use the same guest coordinator.
This replaces manual app closure and sync for normal desktop power operations.
The existing `vm_control stop/reset` remains a separate diagnostic control.

Standard `scripts/run-qemu.sh` launches now allow guest poweroff and restart.
`BROOK_REBOOT_POLICY=shutdown` explicitly retains stop-on-reboot policy; guest
Restart is then unsupported. The supervisor only enables native power ports
for a recognized Q35 launch with a known reboot policy and ordinary poweroff
behavior. Custom `-no-shutdown` launches and other platforms fail closed.
Real-hardware ACPI power management remains unimplemented.

## Shared guest operation

Use one kernel-owned state machine for launcher, local shell and opted-in VM
bridge requests. Bind an idempotent request ID to the guest boot identity and
requested action. Return that operation's progress on reconnection; never
repeat a power transition because an observation timed out.

1. Enter a shutdown state that rejects new app launches and reports progress.
2. Request normal closure of GUI apps, and await their complete launch lineages,
   including helpers without windows. Give unsaved-document dialogs time to
   resolve. Do not silently discard documents or kill applications on timeout.
3. Report the apps/tasks still blocking shutdown. Offer cancellation before
   final commit, or a separately explicit termination decision. Bound service
   shutdown as well; distinguish desktop apps, headless user jobs and critical
   system services rather than treating all non-kernel tasks as expendable.
4. Quiesce remaining writers and services, stop audio, then flush filesystem
   metadata and device caches. Record errors and stop the transition if a flush
   fails. Prevent new writes between final flush and the power transition.
5. Power off or reboot. Use a platform power implementation with detected
   capabilities; the existing shell's QEMU-specific ACPI port write and triple
   fault reboot are not a general real-hardware power API.

A busy application must not block the compositor or MCP request handler. Run
coordination in a worker and show visible progress, with explicit failure or
cancellation states. During shutdown, keep the lifecycle status transport alive
until the final transition.

## Transport and host acknowledgement

Provide a narrow network-independent opted-in VM control channel for lifecycle
request/status and completed-flush acknowledgement. The review lost all TCP
transmission following a virtio-net timeout; another TCP command cannot recover
that situation. Do not implement this fallback by editing guest kernel memory
or by typing unverified commands into whichever window has focus. Bind the
channel to the selected VM session and guest boot, and validate bounded requests.
It must not expose arbitrary host commands or silently grant guest root access.

Expose a typed MCP shutdown/restart operation and status lookup. Preserve the
existing pause/reset/forced-stop diagnostic controls as distinct operations.
The launcher and MCP invoke the same guest policy; neither bypasses unsaved work
or converts a timeout into an implicit force decision.

For shutdown, separately await QEMU process death and the original host launcher's
terminal result. For restart, require a new guest boot identity and successful
transport handshake, not merely QMP acknowledgement. Honor the launcher's reboot
policy explicitly. Preserve operation artifacts after shutdown. Optional fsck
is read-only and only runs after all image users have exited.

Panic or an unresponsive kernel may make orderly shutdown impossible. Report
that condition distinctly and retain the explicit unsynced force-stop option;
never label it a completed guest sync. A successful flush also does not promise
power-loss durability beyond the filesystem/device guarantees.

## Acceptance

Cover empty desktop, both browsers with helper tasks, ordinary background jobs,
unsaved Writer/GIMP edits, cancelled close dialogs, hung app, stopped audio,
flush failure, duplicate requests, request timeout/reconnection, stale boot and
session IDs, broken networking, and panic/unreachable guest. Verify no new launch
or writer can race the final flush. Test launcher and MCP requests from both
user-started and agent-started opted-in VMs. Retain original launcher exit status,
serial/device audit and read-only filesystem checks. Test restart under normal
and no-reboot QEMU policies and document real-hardware support separately.

## Implemented coordinator

The kernel worker in `shutdown.cpp` blocks new launcher/structured command
admission, cancels queued app launches and awaits dispatches already started.
It requests cooperative window closure on the compositor thread. Save dialogs
remain interactive: each initial window and late new client group receives its
normal close request, while later dialogs from a known group are preserved.
Unknown window origins, lingering headless jobs and uncooperative applications
block progress; timeouts never become an implicit kill. Use Cancel while the
operation is closing applications. Cancellation reopens admission and leaves
remaining documents/dialogs available; already-closed applications are not
restarted. Same-client windows opened later can require manual closure because
they cannot safely be distinguished from save dialogs with the current API.

After applications finish, the worker closes user-birth admission, awaits
unpublished births, and sends SIGTERM to explicitly tagged service groups. It
selects live, incarnation-checked recipients and prefers an unmasked sibling.
Services that ignore or block termination remain blockers. Cancellation is
rejected once service teardown starts. All remaining user tasks, live address
spaces, queued destruction and active finalizers must drain before final sync.

`DeviceWrite` now admits every in-tree filesystem/partition block write through
one short gate, including the ext4 module. Final closure succeeds only with no
admitted writes; afterward only the persistent coordinator worker can write
during its final `VfsSync`. Other writers receive EROFS. Audio is stopped before
sync. A sync or platform-power error is retained as failure and does not reopen
launches or writes over a partly torn-down desktop. Recovery after that stage
requires inspection and an explicit diagnostic decision; no automatic retry,
forced kill or unsafe cancellation is provided.

MCP `vm_lifecycle` accepts `shutdown`, `restart`, `status`, `cancel` and `commit`.
Requests are bound to a guest boot and retained ID. A start call can proceed to
commit when ready within its wait. If it returns while blocked, use `status`
with the same ID; after `ready`, explicitly `commit`. Status never replays a
start or commit, including after a lost reply. Retained observations remain
available after QEMU exits. Restart invalidates cached references before
dispatch and requires a changed guest boot plus a matching TCP handshake.
Power/status requests travel over private COM2, independently of guest TCP.
The MCP server must be reloaded to advertise the added tool.

The host receipt distinguishes QEMU exit from the original outer launcher:
the latter still needs its own terminal result from the invoking environment.
Panic/unreachable kernels cannot promise orderly shutdown; existing explicit
forced diagnostic controls remain separate. The bounded table retains 64
operation IDs until reboot. Widget-level save-dialog semantics, per-blocker
progress details and full cross-app save testing remain follow-ups.

### Coordinator acceptance (18 October)

All 44 native suites and the compiled entry audit pass. Native fixtures exercise
the actual coordinator's cancellation, late clients versus same-client dialogs,
masked service siblings, pending dispatch/births, retained address spaces,
active finalizers, write-admission contention and sync failure. The actual
`DeviceWrite` test races a held writer against closure, denies late writers and
allows the final owner and nested partition writes. The bridge suite ran 240
tests with five pre-existing PyAV skips; nine retained-operation tests also
passed after the final host receipt change. These are targeted checks, not
general verification of every shutdown path or full bootstrap renewal.

Session `ae0df8ab33ff48cebbc7cc69aa12ac37` used eight CPUs/8 GiB and kernel
SHA-256 `f3ae05d6e1ef10c5fc008d1d4ead0ece50fae89e3b1e07514e8a457e910dd047`.
Mousepad's unsaved document blocked shutdown at its save dialog; cancellation
left it open. Normal-launcher Firefox and Chromium then closed through the
coordinator. Before restart, the independent census showed no user tasks or
windows, and all live-address-space/queued/active cleanup counts were zero.
User births were closed with no pending reservations, heap integrity passed and
audio was inactive. Real guest restart produced a different boot ID and a
matching guest handshake. The rebooted launcher then powered off with `net0`
disconnected. QEMU and original launcher exited zero; all three read-only
filesystem checks passed.

Final session `d45b99aea2d348f28873be8ee1000fcf`, also eight CPUs/8 GiB, used
kernel SHA-256 `73cc2764b1a3b86b8dfc26837b4c6d4a35f5080c51419f06d5930efdf02eaa34`.
This adds the explicit shell-launch rejection and cancellation-notice fix to
the previous candidate. The unsaved-document cancellation check passed again;
an attempted shell launch returned no PID with the shutdown rejection logged.
After cancellation, a UID-1000 tracked command wrote and synced an owned probe,
exited zero, drained both lineages and provided exact stdout/stderr EOF without
drops or gaps. MCP shutdown, readiness, commit and stopped-status lookup all
worked with networking disconnected. QEMU and the original launcher exited
zero, all three read-only checks passed, and eight owned files retained exact
hashes and UID 1000/GID 100. All twelve frozen modules independently reported
ABI 20. Neither session had a serial kernel/device/user fault. An independent
read-only review found no remaining blocker in the inspected fixes.

Evidence: `build/workloads/orderly-power-release-20261018/acceptance-audit.json`,
the two sessions' `review/` artifacts, `build/shutdown-coordinator-build-20261018.log`,
`build/shutdown-bridge-tests-20261018.log` and
`build/shutdown-operations-20261018.log`. Full unsaved Writer/GIMP coverage,
hung-service UI, panic/unreachable acceptance and real hardware remain open.

## Implemented foundation: network-independent flush channel (18 October)

The opted-in VM supervisor now provides a private COM2 Unix socket and passes
its session identifier through `opt/brook/lifecycle` in QEMU fw_cfg. COM1 remains
the serial log. The kernel only configures COM2 when that explicit setting is
present; it does not probe or start this service merely because hardware has a
second serial port. `BROOK_VM_LIFECYCLE=0` disables this addition, including for
launches that already use COM2. Enabling it refuses an existing serial-device or
reserved-argument conflict rather than silently replacing a device.

At the foundation revision this was maintenance transport without application
shutdown. The coordinator above now adds the launcher and `vm_lifecycle` path;
`vm_control stop/reset` remains the existing TCP-based diagnostic operation.
The maintenance verbs below still have their narrower original semantics. No
arbitrary command or kernel-memory-write interface is exposed by COM2.

`src/kernel/src/lifecycle_channel.h` implements the bounded protocol;
`lifecycle_channel.cpp` supplies the optional UART worker. The host transport is
`tools/vm_bridge/lifecycle.py`. The current verbs are:

| Verb | Behavior |
| --- | --- |
| `hello` | Returns the immutable guest boot ID; a fresh host proposal initializes an unset epoch. TCP and COM2 share the same epoch. |
| `sync` | Checks the expected boot ID and runs `VfsSync(nullptr)` once for this request ID; retains the actual success/error result. Writers must already be quiescent. |
| `status` | Returns the retained result for that boot/request ID without running another flush; an unknown request explicitly reports `missing`. |

Frames contain exactly five bounded ASCII tokens: protocol version, selected
VM session ID, request ID, expected boot ID (or fresh proposal for hello), and
verb. Replies identify the request and guest boot, state and signed result.
Malformed/overlong UART lines are discarded through newline. UART error flags
invalidate the line. The UART worker uses bounded receive/transmit work and a
2 ms polling sleep; it is not an interrupt-driven serial service. Its idle
cost, physical UART faults and sustained malformed-stream stress are not yet
performance/real-hardware acceptance claims.

The worker retains 64 flush results until reboot and never evicts an old ID to
execute it again. Capacity exhaustion explicitly reports `full` without running
a flush; existing results remain queryable. This is a reported capacity limit,
not implicit success or an eviction policy. Hello/status do not consume slots.
The host serializes clients on a per-session lock and validates endpoint type,
owner, current VM generation, reply size, request, boot, state and result. It
never automatically retries a timed-out dispatch or substitutes keyboard input.
After a lost reply, query `status` with the original boot/request identity;
`missing` is not proof that the original request will never arrive. Retain
unknown completion and inspect the same operation rather than issuing another
ID. Every hello initialization proposal must be newly generated, never copied
from a persisted pre-reboot operation.

The status transport still requires a running kernel worker. Panic-time QMP
inspection remains separate; this is not the recalled post-panic command pipe.
A raw maintenance flush does not inhibit later writers, close applications or
establish power-loss durability. Use the coordinator's power operations for
orderly desktop shutdown.

### Verified behavior

Private candidate session `0a9fa442b6a84e46863f3351f799b702` used eight CPUs,
8 GiB and kernel SHA-256
`133de7d168a9a6dce984a464066a950e54aba1e620a392aa1d83f6d416401cf2`.
COM2 hello agreed with the existing TCP epoch. With QEMU `net0` explicitly
link-down, TCP requests timed out while QEMU remained running, but COM2 flush
returned a checked success. Retained status survived socket reconnection;
duplicate sync IDs returned the retained result; stale boot IDs were rejected
and unknown request status reported missing. A deliberately discarded sync
reply was recovered by status lookup without sending a second sync request.

The empty desktop was stopped after a **fresh** COM2 flush while networking was
again disconnected. QEMU exited and the original launcher returned zero. All
three read-only filesystem checks passed. Five existing documents retained exact
hashes and UID 1000/GID 100; a sixth owned probe file also matched its exact hash.
The tracked preparation command exited zero with complete output and all its
lineages drained. Heap integrity passed, audio was inactive, and the stopped
serial audit found no kernel/device/user fault. This does not establish the
future application-closing or final-writer-barrier requirements.

All 41 native tests and the compiled entry audit passed. Nineteen transport tests
cover fragmented replies, disconnect/timeout without retry, old replies, stale
boots, invalid results, bounds, endpoint and lock checks, QEMU arguments and
opt-out. The full bridge suite ran 230 tests, with five recording codec tests
skipped because the generic Python runtime lacks PyAV; this change does not
claim new recording acceptance. Nine bootstrap observer tests pass. Both full
bootstrap paths still need renewal for this 41-suite source revision; historical
40-suite acceptance is not relabelled.

Evidence: `build/workloads/lifecycle-channel-20261018/acceptance-audit.json`,
`review/channel/`, `build/lifecycle-channel-build-20261018.log`,
`build/lifecycle-channel-host-20261018.log`,
`build/lifecycle-channel-bridge-tests-20261018.log` and
`build/lifecycle-channel-bootstrap-observer-20261018.log`.

## Next prerequisite: process birth admission

The [process-birth reservation work](process-birth-shutdown.md) now accounts for
user processes between PID allocation and scheduler publication. The future
coordinator closes user admission and waits for those reservations before its
final task census. The gate remains open in ordinary use. Existing processes,
pending launcher requests, services and mapped/file writers are coordinated by
the shared shutdown worker described above. Resource-capacity
acceptance now verifies PID exhaustion returns an allocation failure rather
than panicking, followed by full cleanup and successful forks.

## Service roles and deferred cleanup observations

`run --service` now assigns a lifecycle role before scheduler publication.
The session bus and Wayland startup scripts use it. Fork and thread creation
inherit the role, and exec preserves it; ordinary catalogue applications remain
applications. This classification does not grant credentials or privileges.
An already running `--once` match is not retroactively promoted. Activation
services that launch applications will need an explicit application handoff;
the shutdown coordinator must still close GUI windows regardless of this role.
The appended Process field requires module ABI 20 and matching rebuilt modules.

`vm_stats.cleanup` now reports live user address spaces, queued processes and
address spaces, active finalizers, and cumulative completed destructions. Live
spaces include externally retained and unpublished spaces. Active finalizers
remain counted after dequeue, through mapping cleanup and completion-waiter
release, including a captured consuming-wait batch. These observations are not
a shutdown barrier: admission, live tasks, services and kernel writers must all
be coordinated before the final flush.

Private eight-CPU/8-GiB session `163d971c19c3434a924415197525a08a` used kernel
SHA-256 `b549ea3b4c60e073e76f4f46a5e4ed91368049d29fda74eb7d304353c4e5138b`.
All twelve frozen modules independently reported ABI 20. An owned, bounded
fixture verified six service tasks and six ordinary application tasks across
fork, exec and threads, all UID 1000/GID 100. Both families exited normally;
their exact process incarnations disappeared, and cleanup returned to the two
background service address spaces with no queued or active destruction.

Firefox's normal catalogue launch remained an application across 293 sampled
tasks. Its owned local VP9/Opus fixture produced 441,118 stereo frames at
44.1 kHz over ten host seconds, approximately 440.18 Hz, with no silent 10-ms
bins. Browser telemetry reported approximately 24 video frames per page-second;
this is not independent screen-presentation measurement or general website
acceptance. Pause stopped the audio stream. Normal close drained the complete
launch lineage and returned cleanup to the background-service baseline.
Chromium's separate first-frame issue remains open.

All five tracked fixture commands exited zero with complete output and drained
lineages. Heap integrity passed. Checked shutdown, QEMU exit, original launcher
exit zero and all three read-only filesystem checks passed. Seven existing
owned files retained exact hashes and UID 1000/GID 100. All 42 native suites and
the compiled entry audit passed; the existing VMM suite now checks retained
references, blocked finalizers and concurrent enqueue during batch destruction.
An independent read-only review found no blocker in the inspected role and
counter paths. Full bootstrap renewal remains pending.

Evidence: `build/workloads/shutdown-services-20261018/acceptance-audit.json`,
`review/verified-role-identities.json`, `review/firefox-audio/`, and
`build/shutdown-services-build-20261018.log`.
