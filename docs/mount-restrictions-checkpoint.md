# Restricted bind-remount checkpoint

Unchanged bubblewrap 0.12.0 now runs a child successfully as UID 1000 in Brook.
The accepted invocation is:

```
srt-bwrap --unshare-user --bind / / -- /boot/BIN/NNP --already-restricted
```

The existing NNP probe explicitly expects bubblewrap's inherited restriction.
It passes 25 checks plus its exec child, including thread/fork inheritance and
irreversibility; transition/isolation cases requiring an initially unrestricted
parent remain covered by the separate ordinary NNP regression. Bubblewrap exits
zero with no stderr. This extends [mount reporting](mountinfo-checkpoint.md).

## Implemented policy

Private, noninitial namespaces support `MS_BIND|MS_REMOUNT` with exactly
`MS_NOSUID|MS_NODEV`, optionally `MS_SILENT` and the exact historical magic
prefix. The target must be the top live owned mount root. Restrictions cannot
be weakened. Unsupported bits fail before mutation; read-only, noexec, atime
changes, recursive remount and general Linux remount compatibility are not
claimed. Existing no-privilege-gain behavior satisfies nosuid.

Typed character/block opens check nodev before invoking the backend, except
metadata-only opens. Legacy opens use the same gate. Recognized synthetic
devices use a separate activation check against the deepest existing mount.
Generic metadata and broker eligibility are unchanged: nodev does not prohibit
directories, ordinary `/dev/shm` files or existing device descriptors, including
SCM_RIGHTS copies. Child mounts retain their own flags. Existing bind-root
broker suppression remains separately restrictive.

## Validation

Private evidence: `build/workloads/mount-restrictions-20261023`.
All 100 host tests, VFS/ext2 ASan/UBSan and compiled entry/scheduler audits pass.
Actual-VFS tests verify pre-backend activation rejection, metadata access,
independent aliases, bind and namespace-copy restrictions, child-mount flags,
legacy opens and rejection without mutation.

The identical portable probe passes Linux (91 fixture / 8 parent cumulative
checks) and Brook (78 / 8, including Brook-specific unsupported-flag cases).
It chooses the actual device-containing mount in each topology: Linux's `/dev`
is separate; Brook's synthetic device uses the root attachment. It verifies
new-open denial, existing and transferred FD use, unchanged metadata behavior,
shared memory, namespace inheritance and unchanged parent behavior. Brook's
existing pathname-stat limitations for synthetic devices remain unchanged.
The first Linux probe incorrectly assumed root nodev would affect a `/dev`
submount; that failed unit and its evidence remain separate.

The mountinfo, pivot, recursive bind, tmpfs, propagation, proc namespace,
user namespace and mount probes pass, as do nine filesystem/credentials/IPC
regressions and Mousepad launch/render/close. Original jobs/groups finish empty;
heap integrity, checked shutdown, original VM exit zero and four offline
filesystem checks pass. Linux's accepted unit has MainPID 0, no control group,
success, a clean original VM exit and a clean offline filesystem.

Frozen ABI-43 kernel with 13 matching modules:
`28a31d6a6dc9e6a6f3d654004a10351372f1d384bb842a52a19115be5657679b`.
Native session: `9719b1dba9004b2f9fb05dfd0155c161`.

## Actual SteamRT result

The original runtime `run --batch -- /bin/sh ...` chain now executes as UID 1000
through `pressure-vessel-unruntime` and `pressure-vessel-wrap`. Its real exit
is 1. It warns that following `/proc/self/fd/10` as the home directory is
unsupported, and its mandatory bubblewrap host-layout check fails:

```
bwrap: Can't find source path /usr: No such file or directory
```

The real runtime requests read-only binds during that check; these also need
implementation. SteamRT's child has not executed. No authentication was attempted,
container identity was not fabricated, and no runtime checks were bypassed.
