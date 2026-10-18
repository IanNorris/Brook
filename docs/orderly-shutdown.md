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
