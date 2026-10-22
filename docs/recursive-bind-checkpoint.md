# Recursive bind checkpoint

The Steam working branch implements directory `MS_BIND | MS_REC`, optionally
`MS_SILENT`, and normalizes the exact legacy `MS_MGC_VAL` prefix before flag
validation. Unrelated flags still fail. This extends the earlier
[tmpfs checkpoint](tmpfs-checkpoint.md); it does not implement pivot_root.

The subtree is selected using retained attachment and object ancestry, including
partial-directory sources and stacked mounts. Names are used only to prepare
reporting labels. Source backing data remains shared. Descendant attachments and
virtual boot ancestors are copied using an identity map; inherited detach locks
remain on copied descendants. A new graft root is a fresh bind attachment.
Source/destination overlap is a finite snapshot of the old tree.

Preparation takes place outside the published registry under the existing VFS
mutation transaction. Every edge, object and label is prepared before a single
metadata-view lock window publishes the complete graft. Failure releases all
cross-edges and memoized virtual objects before freeing prepared attachments.
Backend callbacks, allocation and destruction do not run under the view lock.

This remains bounded: directory sources only, private topology, at most 4,096
prepared attachments, existing 64-byte reporting labels and retained-walk depth
limits. Reporting can conservatively reject a retained path it cannot describe.
Replacing the destination namespace root remains unsupported. Broker suppression
is unchanged: recursive bind does not fabricate identity-backed device nodes
from synthetic names. Pivot/root relocation, shared/unbindable propagation,
read-only bind remount and full container compatibility remain separate work.

## Validation

Evidence: `build/workloads/recursive-bind-20261022` (private, ignored).
The identical static `recursivebindcheck` binary passes on Linux 6.18.26 and
Brook: fixture 103 and parent 8 checks (inherited/cumulative counters, not a sum
of independent checks). It covers nested and stacked mounts, nonrecursive versus
recursive behavior, legacy flags, rename, held covered and detached descriptors,
independent detach, partial-directory sources, source/destination overlap,
self-bind, and initial unprivileged denial. The named Linux service exits zero
with no remaining control group; its original VM exits zero and passes offline
filesystem checks.

The full 100-test host suite passes. Additional actual-VFS tests cover each
allocation failure through a recursive root-to-descendant graft with synthetic
boot ancestors, exact allocation/reference rollback, a source on a different
attachment from the namespace root, retained covered paths, inherited locked
descendants, rename, and metadata counts observing complete bind publication.
These tests pass ASan/UBSan. Concurrent count testing applies to graft publication;
existing detach still removes attachment records individually.

Two unsuccessful native trials remain preserved separately. The first found a
null parent-edge dereference when the source attachment differed from the
namespace root; the guard and regression now cover that case. Its halted VM was
force-stopped after saving bounded fault evidence; its tmp filesystem does not
pass offline checks, and those disks are not acceptance inputs. The second
exited the probe honestly at a reporting-coordinate mismatch; using the same
retained reporting helper for source, target and descendant labels fixes it.
That trial shut down cleanly and its disks check clean.

Final acceptance uses fresh private copies of the previously checked image,
ABI 43 and all 13 matched modules. Frozen kernel SHA256:
`ad64df32209581298b4469dc30094ab94cabeb9adfe3a8c266db981eeec6222f`.
Probe SHA256:
`a4c62fe06b2246a6883ce4263d062238e39a9fa39908766c4dab4a4114830f4d`.
Session: `8cc8b9cb72eb46bda4111402d01bd940`.
Review: `build/reviews/recursive-bind-v3-off-gpu-1ms-fixed-mlfq-a-20261022`.

The portable probe, both tmpfs modes, propagation, proc namespace handles,
user-namespace and mount-namespace probes, nine credential/filesystem/IPC
regressions, and Mousepad launch/render/close all pass. Every command group and
the desktop client group is empty. Heap integrity passes. Compiled entry and
scheduler-start audits with mutation checks pass; these are structural audits,
not universal correctness proofs. Checked shutdown, the original wrapper's
zero exit and four clean offline filesystem checks all pass.

## Actual bubblewrap result

Unchanged bubblewrap 0.12.0, SHA256
`91f2b55f25ecff638f0357abb9cc18fa1f010fd9e48e8fb59b96196827052a6c`,
runs as UID 1000 with:

```
srt-bwrap --unshare-user --bind / / -- /boot/BIN/NNP
```

It completes tmpfs creation and recursive self-bind of `newroot`, then exits 1:

```
bwrap: pivot_root: Function not implemented
```

The collecting wrapper exits zero while recording the real application exit 1.
The trivial child has not executed. This is the next observed runtime dependency,
not a Steam Runtime or login acceptance.
