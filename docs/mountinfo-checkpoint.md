# Mount graph and retained directory reporting

The Steam working branch now implements per-task `/proc/self/mountinfo` from
the actual mount graph. It extends [private root relocation](pivot-root-checkpoint.md).
SteamRT acceptance remains outstanding.

Rows carry stable mount IDs, parent IDs, filesystem device numbers, physical
bind roots, namespace mountpoints, filesystem types and recorded restrictions.
Paths escape spaces, tabs, newlines and backslashes. Parent-before-child order
also puts covering stacked mounts after their parents, as required by the
bundled bubblewrap parser. All mounts currently have private propagation.
The source field uses Brook's filesystem identifier: the driver API does not
retain a Linux device pathname. Existing global `/proc/mounts` is unchanged.

Metadata is prepared under the VFS transaction and copied under one short
view lock; allocation, sorting and formatting happen outside that lock.
Namespace retirement and stale process-root snapshots fail explicitly.
Recursive detach removes the whole subtree in one view-lock window, then
releases covered paths and publication references outside that lock.
The snapshot bounds remain 4,096 records, 512-byte physical roots and 64-byte
mountpoints. Signed consumer-ID overflow is rejected. This is a copied snapshot
at open time, not Linux's complete seq-file implementation.

A backing rename updates physical bind roots independently of target labels,
including a pivoted namespace root. Reporting anchors at an unaffected mount's
cached label. Otherwise an unrelated tmpfs rename could change `/tmp` to `/TMP`
by reconstructing the path through the case-insensitive boot filesystem.

Directory FD links now report their retained identity, including O_PATH,
renamed directories and covered directories. Directory opens pin the backend
object; proc acquires independent path references under the FD lock and does
backend reporting after releasing it. It neither pins/releases a descriptor
nor triggers its final close inside the outer proc VFS transaction. Scalar
fallback metadata is captured in the same FD-lock window. The old synthetic
FD readlink shortcut is removed. Following FD magic links and general
regular-file identity reporting remain separate work.

Access to mountinfo follows Brook's existing same-thread-group/incarnation
proc-root policy. It does not add general cross-process inspection permissions.
Current recorded attributes cover tmpfs's intrinsic nosuid/nodev restrictions;
bind remount and general read-only/noexec enforcement are separate work.

## Evidence

Private evidence: `build/workloads/mountinfo-v2-20261023`.
All 100 host tests pass, as do VFS/ext2 ASan/UBSan and compiled entry/scheduler
audits with mutations. Actual-VFS tests include allocation rollback, stacked
mount ordering, source-root rename, pivot-root rename, a legacy spelling alias,
and mountinfo reads racing repeated recursive detach. The unchanged bundled
bubblewrap `parse_mountinfo` code accepts root and escaped stacked-target
fixtures. None of these tests alone establishes SteamRT success.

The identical static Linux probe passes escaped records, device identities,
flags, namespace-copy IDs, renamed roots, retained-target stacking, O_PATH and
covered directory FD reporting, and a readlink/close/FD-reuse race.
Brook passes the same probe (fixture 2,756 / parent 7 cumulative checks;
Linux 5,159 / 7, with different initial mount tables). The FD-race readers
complete 28 successful native observations and eight on Linux across 500
close/reuse iterations. This stress does not prove every possible interleaving.

The pivot, recursive-bind, tmpfs, propagation, proc namespace, user-namespace
and comprehensive mount probes pass, along with nine filesystem/credentials/IPC
regressions and Mousepad launch/render/close. Every original job and desktop
client group is terminal and empty; heap integrity, original launcher exit and
four offline filesystem checks pass. The Linux unit finishes successfully with
MainPID 0 and no control group; its original VM also stops and checks cleanly.

Unchanged bubblewrap 0.12.0 now reads mountinfo and resolves its destination FD.
It exits 1 at the next genuine dependency:

```
bwrap: Can't bind mount /oldroot/ on /newroot/: Unable to remount destination "/newroot" with correct flags: Invalid argument
```

Its trivial child has not executed. The collecting wrapper's exit zero is not
application success or SteamRT acceptance. Native session:
`a01f6cb347a94f0bab751ceef1c04a82`.
Review: `build/reviews/mountinfo-v2-off-gpu-1ms-fixed-mlfq-a-20261023`.

Frozen kernel ABI 43, 13 modules:
`dc385097bb2d757cb192b70936e679bb84f0f55b263e538eaa3dabbd93231c3f`.
Probe SHA256:
`94e723a3b06f42449602f90004531c4effb9f78e5c84ec574bd6ebf5533c06c5`.

The first native attempt and diagnostic repeat exposed the `/tmp` spelling
regression after rename; their failed job results are preserved separately.
Both VMs stopped cleanly and each passed four offline filesystem checks.
