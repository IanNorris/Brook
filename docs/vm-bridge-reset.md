# Verified VM reset — acceptance, 17 October 2026

Bridge 0.9.1 corrects a real reset failure with the experiment launcher's
`-no-reboot -no-shutdown` flags. QEMU can acknowledge `system_reset` while
converting it into shutdown. Its reset request latches that decision at dispatch;
resetting a stopped VM may subsequently enter prelaunch. See the
[QEMU reset implementation](https://github.com/qemu/qemu/blob/master/system/runstate.c)
and [QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).

## Implemented contract

The new launcher records an unambiguous reboot policy. After the existing checked
sync, reset temporarily selects `reboot=reset`, journals intent, changes host
generation and invalidates cached input/frame references **before** sending one
reset. Unknown delivery is never automatically retried. On acknowledgement it
restores the launch policy immediately through the same QMP connection, changing
neither panic nor shutdown policy. A failed connection is closed before opening
a fallback restoration connection: QEMU's private monitor accepts one client.

Bounded polling resumes only eligible prelaunch/paused states, at most once,
and honors takeover/cancellation. It observes current QMP state and initializes
fresh zero epochs using the existing first-wins protocol. A different nonzero
boot identity plus running QMP proves restart at the kernel/debug-transport
level, not desktop or app readiness. Without an accessible previous epoch,
`running_unverified` reports the weaker evidence explicitly.

`vm_reset_status` recovers the latest attempt without reset/resume/policy changes.
Historical verification is separate from current VM state and survives pause or
exit. An unset epoch may be initialized as protocol bookkeeping. Cancelled or
never-sent attempts are not relabelled pending. Stale host-generation
correspondence is explicit. Restoration errors remain separate from boot evidence.
The endpoint does not automatically repair a policy or replay an action.

Older registrations/custom policy configurations are refused when their original
policy is unknown. Direct external QMP policy changes are outside the recorded
launch-policy contract. Caller-ID reset deduplication, full attempt history and
process-crash recovery of a temporarily changed policy remain future work.
`wait_s` bounds readiness polling, in addition to sync/dispatch and bounded
transport cleanup; it is not a total RPC wall-time deadline.

## Host tests

All 199 bridge tests pass; five optional encoder tests are skipped. Twenty-two
reset tests cover acknowledged versus unknown delivery, generation/cache
invalidation before dispatch, zero-epoch initialization, missing old identity,
policy failures/restoration, single-client connections, delayed prelaunch,
allowed/disallowed resume states, cancellation, takeover, shared epoch-read
budget, historical paused status, stopped/stale/never-sent recovery and clearing stale
current observations after transport loss without erasing historical proof.
The server's existing checked-sync/force refusal tests still pass.

## Real VM acceptance

Session `7b2599b211fe43d1bdfc0ca5125d3618` reused private checked workload disks:
eight vCPUs, 4 GiB, software rendering, 1920×1080, host speakers disabled. Kernel
SHA-256 remained `20f62b2d0c78010b51c966dea595c15f5007961cacf4c0aef68a0393a0e0ba30`.
No kernel or guest application changes were required.

The first implementation reached a new boot but failed restoration because it
opened another QMP client while retaining the first. That failed evidence is
preserved in `reset-acceptance/reset-initial-restore-timeout.json`. The original
policy was explicitly restored without another reset, then the connection fix
passed a repeat normal reset in 7.11 seconds, including checked sync and boot
verification. This was one observation, not a performance guarantee.

The repeatable `tools/vm_bridge/accept_reset.py --panic` then verified:

- Normal checked-sync reset, acknowledged once, new host generation and different
  initialized guest epoch, original reboot policy restored: 7.64 seconds.
- Pausing the new guest preserves historical successful-reset evidence while
  current readiness correctly becomes false. Resume is explicit.
- After a separate checked flush, a controlled kernel panic makes unforced reset
  refuse without changing generation. Explicit forced reset returns the guest
  in 7.41 seconds. It reports `running_unverified`, because the previous epoch
  could not be read from the panicked guest; no false changed-epoch claim.
- All selectable observation components, including completed-frame capture,
  work on the recovered boot. Doctor advertises bridge 0.9.1 and reset revision 1.
- Checked final shutdown acknowledges filesystem/device flush; launcher exits
  zero. Saved reset status remains readable after process exit.

Evidence is under ignored
`build/workloads/action-workloads-20261017/reset-final-acceptance/`.
The full serial log retains the deliberately induced panic. The post-recovery
boot segment is screened independently for user/kernel faults, allocator
exhaustion and device/exit timeouts. Read-only data/Nix/FAT checks after process
exit pass with status zero. These checks establish this reset path, not broader
application, filesystem crash-recovery or full bridge-spec completion.
