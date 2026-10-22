# Private mount propagation checkpoint

This bounded phase implements PRIVATE and SLAVE conversions within Brook's
entirely private mount topology. It does not implement shared peer groups,
master/slave relationships, unbindable mounts, tmpfs or root relocation.
Native acceptance and the unchanged bubblewrap result are recorded below.

## Why a validation-only operation is correct here

Every Brook mount construction, bind and namespace-copy path produces private
attachments. There is no peer group, master or propagation link to preserve.
Linux leaves a private mount private when asked to make it slave without a
master; converting it to private also leaves it private. Applying either
operation recursively has the same result for every descendant.
See [Linux change_mnt_propagation and do_make_slave](https://github.com/torvalds/linux/blob/v6.12/fs/pnode.c#L69).
This is a supported identity conversion with validated preconditions, not
acceptance of arbitrary mount operations. Introducing shared or master-linked
attachments must replace this private-only contract first.

The syscall accepts exactly one of MS_PRIVATE/MS_SLAVE, optionally MS_REC and
MS_SILENT. Shared/unbindable conversions, mixed propagation types and unrelated
flags remain rejected. Source may be null; a supplied source string is checked
as userspace memory but is not resolved as a filesystem path. Filesystem and
data operands do not select a new filesystem in this operation.

CAP_SYS_ADMIN is required in the mount namespace's owning user namespace.
Under the VFS mutation transaction, the selected attachment must be published,
owned by that namespace, and the target must be its exact mount-root object.
Ordinary directories, detached roots, foreign attachments and retired namespaces
cannot pass this check. Namespace root and inherited locked attachments are
valid targets; conversion never unlocks inherited detach restrictions.
The operation performs no allocation or backing callback and preserves retained
paths, mount count, content and publication identities.
See [Linux propagation flag and target validation](https://github.com/torvalds/linux/blob/v6.12/fs/namespace.c#L2531).

## Differential and regression evidence

Private evidence is in `build/workloads/mount-propagation-20261022`.
The portable probe is `tools/at-syscall-probe/mountpropagationcheck.c`.
The identical static binary is used on Linux and Brook:
`fc3c77405c6fc6ebad75f696ef5bd33f042f64bf041c7084f21492f4c248eb75`.

The Linux reference first makes its test tree recursively private, establishing
the same invariant as Brook even if the host-derived tree had shared mounts.
It tests root and nested bind roots across PRIVATE/SLAVE, REC and SILENT;
null/nonexistent source; ordinary-directory rejection; invalid combinations;
unprivileged denial; inherited detach locks; and later binds isolated in both
parent-to-child and child-to-parent directions. Its named service terminates
successfully with MainPID 0 and no control group. The original reference VM
shuts down through ACPI and passes offline filesystem checks.

All 99 host tests pass in the Nix development environment. Six affected
ASan/UBSan tests pass. The retained-VFS fixture additionally tests prepared and
retired namespace rejection, foreign attachment rejection, nested mount roots,
detached targets, unchanged inherited locks and allocation-free validation.
Compiled entry/start audits with mutations pass on the frozen kernel. These
structural audits are not universal handler or scheduler correctness proofs.

## Native acceptance

The identical Linux-tested binary passes on Brook with the same cumulative
per-process counters (parent 6, private fixture 153, inherited-lock child 172;
these are not independent totals to add). The existing proc namespace probe
also passes its ordinary and cross-UID fixtures.

The frozen kernel SHA-256 is
`f137e1faa666832beefa8c98777bdb11f92bbe2d68aa1ab4e884ea47ae9d8c46`,
with ABI 42 and all 13 matched modules. Native session
`4cae0954bb304d80aab965e3dab46a0c` retains original job identities in
`build/reviews/mount-propagation-off-gpu-1ms-fixed-mlfq-a-20261022`.

The proc, user-namespace and comprehensive mount regressions pass, along with
nine credential/filesystem/IPC regressions. Mousepad launches, renders and closes
with its registered client group empty. All command jobs finish with no live
tasks. Heap integrity passes, checked shutdown completes, the original launcher
returns zero, and all four offline filesystem checks return zero.

The unchanged shipped bubblewrap 0.12.0 executable (SHA-256
`91f2b55f25ecff638f0357abb9cc18fa1f010fd9e48e8fb59b96196827052a6c`)
runs as UID 1000 with the identical arguments:

```
srt-bwrap --unshare-user --bind / / -- /boot/BIN/NNP
```

It advances past propagation and exits 1 at:

```
bwrap: Failed to mount tmpfs: Invalid argument
```

The collecting wrapper exits zero while retaining the application's real exit 1
and stderr. The trivial child has not executed, and no descendants remain. Tmpfs
and root relocation are subsequent work. Steam authentication and general
container-security acceptance are outside this checkpoint.

The subsequent [fresh-instance foundation](tmpfs-foundation-checkpoint.md) adds
independent backing publication for a future memory filesystem. It does not
resolve the tmpfs failure recorded here.
