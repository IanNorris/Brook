# Controlled memory-exhaustion recovery

Branch: `wip/oom-recovery-20261029`, based on the accepted paging/compression
branch. Implementation is in progress: the coordinator is not enabled yet.

## Presentation and preference checkpoint

`tools/brook-message-pkg` builds a standalone user-mode GTK app with native
window decoration. A stable error code selects concise language; an expandable
technical section includes identity, executable and valid measured numbers.
`brook-message --code app-closed-memory --app GIMP` is a manual invocation;
`--event-file` accepts one bounded versioned `BrookSystemMessage` record, and
`--print` exercises formatting without a display. No launcher entry is added.

Settings now saves `[Experience] technical_details=true/false` alongside the
existing settings. Simple messages are the default; technical details remain
available in either mode. The checkbox affects presentation only. Both programs
honor the existing `BROOK_USER_SETTINGS` override and otherwise use
`/data/home/.config/brook/settings.ini`.

The message record distinguishes unknown/disabled/configured/full experimental
paging. A measured increase in global free RAM is explicitly not attributed
entirely to the victim. The separate application-released field is displayed
only when its measurement flag is supplied. Do not populate it with a simple
before/after global-free subtraction.

Both Nix packages build. The message derivation checks preference parsing
(missing, malformed, true/false and unrelated settings), and the hosted formatter
checks exact simple wording, measured/unknown state, contradictory telemetry,
unsupported records and buffer bounds. Native UI/persistence verification and
automatic event delivery remain required.

## Kernel foundations

`memory/oom_policy.h` is the allocation-free policy for the future prestarted
worker. It requires a bounded reclaim opportunity, rechecks pressure before
reserving a victim, serializes one pending victim, applies cooldown/hysteresis
and enters a stalled state rather than cascading when cleanup misses its
10-second deadline. Initial low/high thresholds are 8/32 MiB; selection timing
must be validated in real low-memory workloads. Kernel/service, exiting and
cross-group shared-AS candidates are excluded. This policy has hosted tests but
is not yet connected to allocation failures.

`VmmMeasureRangeStep` measures at most 4,096 page-table entries per batch,
allowing scoring to release AS locks between batches and resume by address.
The additional exclusive-managed-page count excludes CoW leaves, multiple user
mappings and retained extra references. Callers must restrict it to managed
private RAM; it is a conservative scoring snapshot, not a lifetime pin or a
promise of memory released. Real VMM tests verify shared-page exclusion and
bounded traversal across large holes/multiple batches.

A resident `AddressSpaceDestructionWatch` can observe one recovery victim
without holding an AS reference that would prevent its destruction. Registration
requires an existing AS pin; cancellation/completion share the cleanup guard.
The hosted actual-finalizer test delays TLB acknowledgement, verifies completion
remains false, then verifies completion and safe observer reuse/cancellation.
This watches completed AS teardown; PMM quarantine drainage and global headroom
must still be observed independently.

## Remaining integration and gates

- Publish allocation pressure without allocating, waiting, taking scheduler
  locks or killing inline. Distinguish fragmentation/oversized requests and
  ordinary app ENOMEM from global exhaustion. Preserve normal failure returns.
- Prestart the resident worker and bounded emergency resources. Reclaim clean
  cache and permit quarantine drainage before selecting an application.
- Obtain referenced candidates under registry protection, score bounded mapping
  batches, and atomically revalidate incarnation/AS/roles/all sharing users with
  the death latch. Skip cross-group shared ASes for the first version. Use
  existing asynchronous fatal termination and protected-operation unwind.
- Keep the cleanup watch alive through a stalled episode, release temporary AS
  and process refs before waiting, and report actual completion/headroom.
- Add a bounded immutable event queue, safe user-copy API, MCP evidence and a
  user-mode presenter that can deliver retained events after headroom recovers.
  Protect presenter/service lifetimes; presenter failure cannot recursively
  initiate another OOM episode.
- Fail new app admission transactionally and expose a launch-memory message.
- Test real scheduler selection-versus-clone/exec/exit/PID reuse and protected
  service/shared-AS exclusions; simultaneous failures, genuine exhaustion,
  reclaim success, no eligible victim and stalled cleanup without kill storms.
- Fresh private native low-RAM runs with paging disabled and backing exhausted
  must preserve the desktop/control plane, show the truthful user-mode message,
  recover usable memory, launch another app, and finish checked shutdown/fsck.

Current build/test receipts are ignored files prefixed `build/oom-`. No native
OOM survival, automatic victim selection or native message screenshot is claimed
by this initial checkpoint.
