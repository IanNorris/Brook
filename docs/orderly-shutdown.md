# Orderly desktop shutdown and restart

Proposed after the 18 October live browser review. The launcher needs Shutdown
and Restart actions, and MCP needs the same lifecycle operation. Existing
`vm_control stop/reset` synchronously flushes metadata before QMP, but expects
callers to close applications first and depends on the guest TCP debug channel.
This is not yet a complete desktop shutdown feature.

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

## Implemented foundation: network-independent flush channel (18 October)

The opted-in VM supervisor now provides a private COM2 Unix socket and passes
its session identifier through `opt/brook/lifecycle` in QEMU fw_cfg. COM1 remains
the serial log. The kernel only configures COM2 when that explicit setting is
present; it does not probe or start this service merely because hardware has a
second serial port. `BROOK_VM_LIFECYCLE=0` disables this addition, including for
launches that already use COM2. Enabling it refuses an existing serial-device or
reserved-argument conflict rather than silently replacing a device.

This is maintenance transport, **not yet orderly application shutdown**. The
public `vm_control stop/reset` path is still the existing TCP-based operation.
Launcher actions, the shared shutdown coordinator, launch inhibition, writer
quiescence and platform power handling remain to implement. No new arbitrary
command or kernel-memory-write interface is exposed.

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
A completed flush does not inhibit later writers, close applications or establish
power-loss durability. Future MCP/launcher lifecycle integration must provide
those coordinator guarantees before using this as an orderly shutdown API.

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
coordinator can close user admission and wait for those reservations, then take
its task census. The gate remains open in ordinary use; this is not yet wired
into launcher/MCP shutdown. Existing processes, pending launcher requests,
services and mapped/file writers still require coordination. Resource-capacity
acceptance now verifies PID exhaustion returns an allocation failure rather
than panicking, followed by full cleanup and successful forks.
