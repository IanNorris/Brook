# SteamRT container feasibility gate

Status (2026-10-22): the bounded mount-namespace and
[user-namespace foundation](user-namespace-checkpoint.md) checkpoints are accepted
after source review, Linux differential probes and native regression/teardown
checks. The subsequent [unchanged bubblewrap checkpoint](bubblewrap-checkpoint.md)
fixes missing overflow-ID reporting. The accepted
[proc namespace-handle checkpoint](proc-namespace-handles.md) then advances the
same executable to its mount-propagation operation. The accepted
[private propagation checkpoint](mount-propagation-checkpoint.md) advances it
to tmpfs. The [fresh-instance foundation](tmpfs-foundation-checkpoint.md) and
[bounded tmpfs backend](tmpfs-checkpoint.md) then supply independent memory
filesystems with accounting and explicit mapping rejection. The subsequent [recursive bind checkpoint](recursive-bind-checkpoint.md) copies
nested and stacked mount subtrees and advances unchanged bubblewrap to
`pivot_root: Function not implemented`. The subsequent
[private root-relocation checkpoint](pivot-root-checkpoint.md) implements both
pivot forms and advances the same binary to missing `/proc/self/mountinfo`.
The [device bind flags checkpoint](device-bind-flags-checkpoint.md) then accepts
NOSUID-only restrictions. With a real store-visible coreutils PATH, unchanged
SteamRT passes its precheck and reaches F_ADD_SEALS during runtime setup.

The [read-only bind checkpoint](read-only-bind-checkpoint.md) enforces RO with
conservative data/writeback admission and advances unchanged SteamRT to its
NOSUID-only device bind remount. Its requested child has not executed.

The [directory FD checkpoint](proc-directory-fd-checkpoint.md) supplies retained
proc FD traversal and a real /usr skeleton; unchanged SteamRT now reaches the
read-only /etc bind remount. Its child has not executed yet.

The [mount graph reporting checkpoint](mountinfo-checkpoint.md) then supplies
mountinfo and retained directory FD readlink reporting. Unchanged bubblewrap
advances to bind remount restrictions. The [restricted-remount checkpoint](mount-restrictions-checkpoint.md)
then runs unchanged bubblewrap and an NNP/exec child successfully. The real
SteamRT launcher exposes retained directory FD following, missing `/usr` layout
and read-only binds. Write/mapping enforcement needs separate acceptance.
There is no Brook SteamRT-child or login acceptance.

The accepted baseline is `3bfc275e` (ABI 39), including retained filesystem
contexts at `e626b7f8`, retained proc-root links and real per-task no_new_privs.
The historical observations below describe the earlier feasibility gate; consult
[the client notes](steam-client.md) for the latest native acceptance and blocker.

## Evidence that changes the next step

The official native64 client now passes semaphore creation, signal watching and
D-Bus credential authentication. With the legacy STEAM_RUNTIME variable removed,
the original UID-1000 job exits 2, with no live tasks, at:

```
pressure-vessel-wrap: opendir(/proc/self/root): No such file or directory
```

The separately executed requirement checker fails PR_SET_NO_NEW_PRIVS;
unshare(CLONE_NEWUSER) is ENOSYS. These are independent gaps. Supplying a root
link would advance discovery, but would not establish a usable container.

The retained launch chain is pv-run.sh → srt-client-container → runtime entry
point → run → pressure-vessel-unruntime → pressure-vessel-wrap. SDK and custom
runtime options retain this chain. Astra's script review finds no supported
native64 non-container route. The legacy STEAM_RUNTIME=0 branch is not one.

## Historical feasibility boundaries

These are pre-foundation source observations, not the current implementation
status or an exhaustive trace of Steam's requirements. Later checkpoints below
supersede the process-root, privilege-ceiling and mount-namespace rows.

| Area | Current state | Required acceptance before claiming support |
| --- | --- | --- |
| Process root | procfs exposes exe/cwd, but no root entry | Open/readlink/stat of the actual retained process root; correct access, identity and lifetime |
| Privilege ceiling | prctl SET/GET_NO_NEW_PRIVS rejects with EINVAL | Validated, irreversible state; inheritance across clone/exec; privilege acquisition cannot bypass it |
| User namespaces | No user-namespace object or uid_map/gid_map machinery found | Separate ID mappings, scoped privilege checks, mapping restrictions and ownership/lifetime |
| Mount namespaces | VFS uses one global eight-slot mount table | Isolated namespace ownership and lookup; real bind/mount/root transitions without altering the desktop namespace |
| Capabilities | Non-root empty capability subset; root-capable changes unsupported | Genuine namespace-scoped privilege/drop behavior needed by the observed launcher, without success stubs |
| clone/clone3 | Existing clone dispatch does not validate namespace flags | Unsupported isolation requests must be rejected; implemented requests must actually isolate before publishing a child |

Relevant implementation files are procfs.cpp, syscall.cpp, process.cpp/h and
vfs.cpp/h under src/kernel/src. The global mount table makes this an architectural
change, not a sequence of independent syscall-table additions. Existing root
descriptors and cwd/openat references must retain the correct objects through
rename, detach, exit and namespace teardown.

## Bounded first phase

1. Establish the minimum actual request sequence. In a private Linux comparison
   guest, trace the same native64 payload and its unchanged runtime launcher up
   to the pre-authentication UI. Keep the trace private and omit account data.
   Record exact bubblewrap/pressure-vessel versions and syscall arguments. Do
   not infer the full needed namespace/capability set from the first failure.
2. Turn that sequence into a dependency table. Distinguish mandatory calls from
   optional feature probes and real documented fallbacks. Identify namespace
   flags, mount operations, procfs accesses and privilege transitions actually
   exercised. No source-level checks or container markers are bypassed.
3. Review a concrete object/lifetime design with Astra before kernel changes:
   process namespace references, root/cwd references, mount backing ownership,
   UID/GID mappings and privilege checks. State lock order and failure unwind.
   Account for the kernel/module ABI if process or module-facing layouts change.
4. Estimate and choose the implementation boundary from that evidence. If the
   minimum is substantially larger than a bounded phase, preserve the trace
   and stop with the dependency list. Do not repeatedly rerun Steam against the
   same unsupported interfaces.

This phase ends with a reviewed dependency/design decision, not a promised login
screen. It does not start game launching, PID/network namespace development,
seccomp, accelerated Xwayland, scheduler changes or further GPU/audio tuning
unless the trace establishes a mandatory dependency and scope is updated.

## Gates for a later implementation phase

Each implemented object needs differential Linux probes and native Brook
acceptance. Exercise invalid arguments, permission failures, concurrent use,
fork/exec inheritance, last-reference teardown and allocation-failure rollback.
Namespace tests must prove that one process cannot mutate another namespace's
mounts or escape its effective root through absolute paths, .., retained dirfds
or procfs links. Test the legitimate retained-reference cases separately;
chroot alone is not a general isolation mechanism.

Advance from small namespace/root probes to an unchanged bubblewrap command
running a trivial child, then the real pressure-vessel launcher, then the same
native64 Steam client as brook. A fake successful unshare/mount/prctl, a fabricated
container identity, a replacement login website or guest-root Steam does not
pass any gate. Reject unsupported operations explicitly.

Use fresh private copies of the checked Steam image, retain original VM/job
identities, and stage only while all VMs are stopped. Continue checking IPC
regressions, exact client-group cleanup, heap integrity, guest sync and offline
filesystems. Stop when the real Steam login page is visible and responsive;
authentication remains manual. Payloads, traces containing private paths or
account data, and writable runtime state stay outside git.


## Approved mount-namespace checkpoint

This phase ends at a tested namespace-isolation checkpoint and a new estimate,
not Steam login. It preserves the checked ABI-39 baseline and its filesystem,
credentials and IPC acceptance. No user namespace, UID/GID maps, namespace-local
capability model, pivot_root, propagation groups, tmpfs, seccomp or authentication
is authorized by this checkpoint.

### Minimum externally visible behavior

- Ordinary fork and clone inherit the existing mount namespace. A supported
  privileged `clone(CLONE_NEWNS | SIGCHLD)` creates a separate mount topology
  before publication; the parent keeps its original topology. Exec preserves
  namespace membership. Reject incompatible NEWNS/CLONE_FS and unsupported
  namespace combinations before creating a child. Initial-namespace privilege
  checks remain in force; namespace creation never changes numeric credentials.
- Implement directory `MS_BIND` without recursive copying, and `umount2` with
  `MNT_DETACH`, sufficient to demonstrate real namespace-local visibility changes.
  Validate every supplied flag; unsupported mount classes fail honestly. Both
  operations require the existing initial-namespace administrative authority.
  Do not expose an unprivileged substitute for the future user namespace model.
- Copy existing attachment topology without remounting filesystem drivers or
  reopening roots by name. Backend writes remain shared between namespaces:
  topology isolation is not a filesystem data snapshot.
- Preserve existing kernel/driver mount wrappers against an explicit initial
  namespace. Root directory enumeration and proc mount reporting must describe
  the relevant namespace, rather than leaking the initial global table.

### Design constraints established from current code

`MountEntry` currently combines backing ownership, attachment identity, topology,
labels and live path/vnode counts in one of eight static slots. The retained
walker, mutation reservation checks, rename labels, directory enumeration, proc
reporting and global sync all consult that table. Changing only sys_clone would
leave lookup global and does not satisfy this checkpoint.

Separate filesystem backing (driver ops/private data/device and lifetime) from
mount attachments (mounted root object, covered parent object, attachment
identity and lifetime). A bind attachment can start at a subdirectory, so its
mounted root cannot be inferred by calling the filesystem's root_object.
Namespace topology owns attachment membership; referenced paths and vnodes pin
attachments/backing independently of membership. Do not allocate one new driver
mount for each copied attachment or bind. Global sync must still reach live
detached backing, and a single namespace's exit must not unmount another's data.

Use an immutable owned namespace reference inside FsContext so root, cwd, mask
and namespace can be snapshotted consistently through the existing per-process
context publication helpers. CLONE_FS shares that complete context; ordinary
fork copies the context while retaining its namespace. Clone-with-NEWNS builds
the namespace and a private FsContext together,
remapping root and cwd by old-to-new attachment identity before child publication.
Do not copy namespace ownership through Process memcpy. Publish a fully prepared
child or unwind all references through the existing unpublished-child path.

Retained inherited dirfds are different from the remapped root/cwd. Preserve
their legitimate Linux behavior rather than rebasing them from display names or
revoking them to make an isolation test pass. Differential probes must establish
mount crossing and dotdot behavior for these foreign retained attachments before
choosing the final walker/ownership representation. Namespace ownership must not
create a namespace/attachment/covered-path cycle that prevents final release.
Virtual boot mount ancestors embed parent VfsPaths today; copied topology needs
an explicit remapping design for those objects too.

Shared backing rename is a cross-namespace reporting hazard. Existing mount
labels cannot be updated in the caller's namespace alone, because another tree
may attach the same renamed backing elsewhere. Prefer converting remaining
legacy initial-namespace name dispatch to retained traversal; otherwise changes
must preflight/update all affected live labels under the global transaction.
Do not let stale labels become lookup authority or accept a rename before a
required reporting update can succeed.

Keep the current sleeping VFS mutation transaction as the initial serialization
boundary unless review demonstrates a required change. Separate namespaces do
not require per-namespace parallel mutation in this first phase. No scheduler,
process-publication or IRQ spinlock may span backend callbacks, filesystem work
or final reference release. Any registry for live backing/attachments must have
explicit snapshot ownership and teardown; a dangling namespace back-pointer is
not acceptable. The precise lifetime representation is a review gate, not an
implementation already claimed complete.

### Acceptance gates

1. Differential Linux probe for inherited dirfd/root/cwd behavior across NEWNS,
   bind, detach and nested/stacked mount traversal. Check the actual host fixture's
   permissions; use a disposable Linux guest if host isolation forbids NEWNS.
2. Host tests of the chosen topology/backing ownership and copy/remap model:
   independent trees, shared backing, more than eight attachments, stacked mounts,
   last-reference release and deterministic allocation-failure unwind. Exercise
   namespace destruction with open paths/vnodes and copied virtual ancestors.
3. Native root-only namespace probe: parent/sibling topology unchanged; child
   bind visible only where shared namespace membership allows; fork/exec preserve
   membership; retained references survive lazy detach and namespace exit; fresh
   absolute lookup no longer reaches a detached mount. Include root/dotdot and
   symlink-negative tests, parallel mutations and repeated create/exit cycles.
4. Rerun retained FSCTX/proc-root/file-lifetime/relative-at/cwd, credential,
   signalfd and SysV tests plus representative desktop launch/render/close.
   Check matching kernel/modules, heap integrity, checked shutdown, original
   jobs/wrapper termination and offline fsck on private checked copies.
5. Review the accepted changes and remaining gaps, then estimate the next phase.
   A partial model or green unit tests alone do not establish namespace support,
   container security, or Steam login.


### First Linux lifetime discovery (2026-10-22)

The static `mountnamespacecheck` probe ran in a fresh private Linux 6.18.26
reference VM. Its bounded named service used a private user/mount namespace
fixture (`unshare --user --map-root-user --mount --propagation private`) because
this image deliberately has no passwordless sudo. That outer fixture is not a
claim of Brook user-namespace support. Versions B, C and D complete successfully;
D reports 39 child checks and 41 parent checks (counts include shared setup).
Versioned source/binary hashes and original service receipts are retained under
`build/workloads/mount-namespace-20261022`.

Observed Linux behavior:

- After parent and child independently bind B and A over the same target, the
  child's absolute lookup reads A. A pre-clone parent dirfd traversing the target
  reads B from its original attachment tree. A pre-clone dirfd directly on the
  covered target still reads T from the original underlying directory.
- Lazy-detaching a bind with a nested bind makes fresh absolute lookup read T.
  A held detached-root dirfd still reads A, but traversing its former nested
  mount sees ENOENT. A separately held nested-root dirfd still reads B.
- Dotdot at the held detached bind root stays at that root; it does not return
  to the former covered parent or expose the backend subtree's physical parent.
- In the Linux-only lifetime extension, a worker creates a namespace and binds
  A, then clones a follower into a copied namespace. After the worker unshares
  again, no task remains in its original namespace. The follower's inherited
  original-namespace dirfd now reads T; the follower's absolute lookup still
  reads A through its own copied mount. Namespace publication therefore must
  not remain alive just because a foreign dirfd retains an attachment.

Astra reviewed these results. The implementation must distinguish task membership,
namespace storage references, and exact attachment/backing references. FsContext
snapshots and unpublished copied contexts retain storage; their refcounts must
not substitute for the number of live member tasks. Last-member departure must
make old namespace edges unsearchable atomically; delayed backing destruction
must not delay that visibility change.

The reviewed graph has namespace registry membership owning attachment refs,
child attachments holding their covered-parent paths while attached, and no
strong child list owned by a parent attachment. Recursive detach removes edges,
clears parent links and preserves externally held attachment/root/backing refs.
The walker explicitly clamps detached-root dotdot; merely clearing a covered
path would incorrectly fall through to the backend's physical parent. Exact
foreign attachment identity selects edges while its original namespace remains
published. A global attachment index is an index, not an owner that can keep
mounts published after namespace membership ends.

Next implementation gate: model these three lifetimes and deterministic failure
unwind in host tests, including a namespace snapshot held across last-member
departure. Review process exit/publication hooks so topology visibility never
waits for final Process or FsContext reference destruction. No kernel namespace
implementation is claimed by this discovery probe.


### Publication primitive and process-hook audit

`mount_namespace_state.h` introduces only the allocation-free state transitions
for prepared/active/retired namespace membership and independent storage refs.
A last-member CAS retires publication synchronously; a retained context/snapshot
cannot revive it. Hosted tests cover independent member leases with shared
context storage, prepared-clone abandonment, snapshot survival across final
member departure, concurrent joins/snapshots/exits, and joining against the last
leaver. This primitive is not wired into the VFS or scheduler yet and does not
establish mount-namespace support.

The process audit identifies four required future member-token paths:
`SchedulerAddProcess` can publish a stillborn thread; `SchedulerMarkTerminated`
performs ordinary terminal publication; `SchedulerKernelOperationBoundary`
completes deferred terminal requests through that function; and unpublished
creation failure reaches `ProcessDiscardUnpublished` or constructor unwind.
`FreeProcessStruct`/FsContext destruction alone is too late because an exited
process can retain references while waiting for a parent or observer.

Astra reviewed the primitive and proposed hooks. Each task must own an explicit
member token and storage hold, independent of FsContext snapshot reference
counts. Exchange the token once on terminal publication, perform only the
allocation-free member departure there, and queue any topology cleanup with
preallocated state. Scheduler/registry locks must never run backend destruction.
The last member's retirement gates both mount entry and attachment-root parent
traversal immediately, including a covered-parent pointer awaiting cleanup.
Mutation admission must check publication under the topology transaction:
operations already admitted may finish before cleanup obtains that transaction;
new operations after retirement must fail, so no edge is added after cleanup.
Full topology and process-hook integration remain acceptance work.

The final primitive tests pass in the normal host build (0.09 seconds) and the
existing ASan/UBSan build (0.17 seconds). Receipts are under
`build/workloads/mount-namespace-20261022/state`. No kernel/module build, ABI
change, live Brook test, topology implementation or scheduler behavior change
is claimed for this component checkpoint.


### Backing/attachment ownership component acceptance

The real VFS now separates a reference-counted `MountBacking` (driver callbacks,
private filesystem state and device) from its mount attachment records. Additional
attachments of an active driver/device retain its backing; they do not mount the
driver again. Sync visits each backing once. Explicit final-unmount failure
preserves the attachment and its reference. Private construction rollback that
cannot unmount leaves an unreachable zero-reference backing in the registry,
where checked sync retries retirement and reports failures. Such pending backing
blocks a fresh mount of the same driver/device until retirement succeeds; this
prevents FAT's later cleanup from unregistering a newly mounted drive slot.

Filesystem provider registration pins its table before reading it and then pins
all callback/name owners through the existing permanent module-retention API.
Unregistering a name does not free callback owners still referenced by live or
retired backing. Hosted fixtures have process-lifetime providers and a mocked
retention contract; the actual kernel uses the production module check.

Accepted ABI-39 kernel:
`dcf1e2447cb9fe4a439b44fcdcc0c31559513815a752b2c809333d13e1d6f02f`.
All 97 host tests pass in the final run; VFS paths and real Ext2/FAT tests also
pass under ASan/UBSan. The VFS path regression exercises shared backing without
remount, sync deduplication, exact access after another attachment releases,
final unmount failure/retry, failed private rollback/remount rejection, provider
registration rejection, and allocation failure through the first successful
mount construction. An earlier high-parallel run timed out audio-delivery; the
isolated retry and final four-way full suite both pass.

Private review `mount-backing-off-gpu-1ms-fixed-mlfq-a-20261022` verifies all
13 matching modules, nine original native filesystem/credentials/IPC jobs with
zero live tasks, normal Mousepad launch/render/close, heap integrity, checked
sync/stop, original wrapper exit zero and all four offline filesystem checks.
The frozen capsule and `component-acceptance.json` remain under
`build/workloads/mount-backing-20261022`; the frozen HEAD and source diff are
code provenance (the reused CMake build banner has an older cached git label).

This accepts backing ownership only. Attachment records still use the initial
eight-slot topology; dynamic attachments, process namespace ownership, clone
remapping, namespace retirement, bind/detach syscall dispatch and their isolation
acceptance remain unfinished. No container or Steam login support is claimed.


### Dynamic attachment storage component

The initial namespace registry now uses stable heap-allocated `MountEntry`
records rather than eight reusable slots. Explicit unmount still requires all
path/vnode holds and child attachments to be gone before unlink/free. This
component does not yet implement lazy detach or process namespace ownership.
The traversal guard is a separate depth bound (4096), not storage capacity.

Metadata enumeration uses a dedicated IRQ-safe view lock to copy count/names/
snapshots without recursively acquiring the sleeping VFS transaction from a
procfs callback. Writers hold the VFS transaction before the view lock; view
readers perform no backend callbacks and return no record pointers. Individual
copies are memory-safe, but a sequence of count/index calls is not a coherent
snapshot during concurrent mutation. Indexed scans and count/name enumeration
still scale with registry length; this is not unlimited scalability acceptance.

Both legacy and retained rename label updates allocate a private list of only
the affected records before the backend mutation. Allocation failure unwinds
that list without publishing labels or renaming the backend. View-lock label
publication only copies prepared strings; ancestry scans and allocation stay
outside that IRQ-disabled section. Astra reviewed the heap/lifetime and lock
ordering; its requested concurrent enumeration regression is included.

The real VFS host tests cover 64 same-backing mounts, 32 stacked mounts, retained
handles, allocation-failure construction unwind and metadata enumeration racing
200 mount/rename/unmount cycles. Real Ext2 retained-rename fault injection checks
unchanged backend paths/labels on failure and continues past successful retries
until an entire operation succeeds without an injected failure. The backend's
existing OOM-to-ENOENT error mapping is recorded separately in the backlog.

All 97 host tests and the VFS/Ext2/FAT ASan/UBSan tests pass. The ABI-39 kernel and
13 modules build; supported compiled entry/start audits pass. Frozen kernel SHA:
`48912b7be45fb70590669b5606a232c9ab33fe63e3e41f66cf64ef85a382c386`.
Evidence and frozen source are under
`build/workloads/mount-namespace-20261022/attachments`. No VM was started for this
component; native namespace/integration acceptance remains outstanding. The
build helper now explicitly reconfigures the nested kernel CMake project before
building so its git banner is current, rather than only reconfiguring the root.

Next: introduce owned namespace topology over these records, copy/remap root,
cwd and synthetic ancestor links by identity, add member tokens to actual
process birth/exit, and wire real privileged clone/bind/detach. These acceptance
items remain part of the active goal; the component tests do not replace them.


## Private topology core checkpoint (process integration pending)

`mount_namespace.h` and `vfs_mount_namespace.inc` now implement private prepared
namespace copies, activation/member transitions, directory bind, subtree lazy
detach and deferred retirement over the allocated attachment/backing model.
Each record is indexed globally but edges belong to an explicit namespace;
lookup follows exact attachment/object identities and checks live publication.
A copied namespace receives new attachment identities and remapped synthetic
ancestor objects, root and cwd. Foreign retained paths remain foreign
capabilities instead of being reopened by name. Storage snapshots do not keep
publication alive after the last member departs.

Attachment lifetime uses a combined atomic publication/path/vnode lease count;
diagnostic path/vnode counters are not teardown authority. Astra identified a
last-release window in explicit unmount; the authoritative count is now checked
before backing teardown, with a deterministic interleaving regression. Detached
records retain exact objects/backings until their final lease leaves. The final
atomic decrement never accesses the potentially freed record afterward.
Namespace membership retirement hides edges synchronously; deferred VFS cleanup
removes records and releases covered paths, arbitrary roots and backings in a
sleeping transaction. The existing cleanup worker is notified outside its list
lock. Future scheduler hooks must use wake=false under scheduler locks and wake
after releasing them; those hooks are not integrated by this component.

Copy uses explicit attachment and synthetic-object memo tables, never raw copies
of live reference counters. Fault injection through every allocation until a
complete successful copy verifies empty outputs and unchanged allocations and
registry on failure. Host tests reproduce the Linux foreign-dirfd, stacked bind,
nested detach, detached-root dotdot and last-member publication observations.
Real Ext2 tests additionally keep an exact vnode alive through detach and
namespace retirement, then close it before final backing cleanup/offline fsck.

Shared-backing rename labels are reconstructed from retained covered edges in
each namespace, transforming physical bind roots separately before projecting
visible names. Tests cover copied-to-initial and initial-to-copied rename, plus
renaming a bind's physical source with a nested bind. Allocation preflight stays
before backend mutation and label publication only copies prepared strings under
the metadata view lock. A remaining bounded compatibility restriction is explicit:
a directory rename can fail before mutation when another mount on that backing
has unreportable ancestry. An injected unsupported directory_path regression
checks this fail-before-mutation behavior. File and no-op legacy renames skip
label preflight. This does not claim arbitrary disconnected-directory rename
compatibility.

All host tests passed individually: the full parallel run passed 96/97, with
one audio-model timeout while builds competed for CPU; that unchanged test
passed standalone. Affected VFS/Ext2/FAT/VMM tests pass ASan/UBSan, including the
added real Ext2 namespace case. The ABI-39 kernel and 13 matched modules build;
compiled entry/start audits and mutations pass. Frozen kernel SHA:
`71e5dad5323161eadd4cdf218679e18c3a1fe930b8a9aef99baa8388da2a4a99`.
Native component regression VM `ddfea593d26345c9b45705a4d60f14ed` completed all
nine filesystem/credentials/IPC jobs with no live job tasks. Mousepad launched
through the normal UI, rendered a completed frame and closed with its exact
client group gone. A host screenshot-serialization error was recovered using
the original launch identity; no second app was launched. Heap checks passed,
checked sync/shutdown completed, original wrapper exited zero and all four
offline filesystem checks exited zero. Evidence:
`build/workloads/mount-namespace-20261022/topology` and
`build/reviews/mount-topology-off-gpu-1ms-fixed-mlfq-a-20261022`.

The active goal remains incomplete. There is still no process-owned namespace,
no successful CLONE_NEWNS syscall, no privileged bind/detach dispatch, and no
native namespace isolation acceptance. Initial namespace arbitrary binds and
virtual source binds currently fail EOPNOTSUPP in the core. Process-context
storage, independent task member tokens at birth/logical exit, namespace-aware
reporting and real syscall/native tests are the next required integration step.
Steam/container security and authentication remain outside this checkpoint.


## Process ownership and logical membership checkpoint

FsContext and its snapshots now own immutable namespace storage references along
with root, cwd and mask. Ordinary copied/shared filesystem contexts retain that
namespace; every user task independently joins it with a separately owned
storage/member token. Raw Process inheritance excludes both filesystem ownership
fields. Process layout changes require ABI 40 and a matched set of modules.

The unpublished-only ProcessFsIsolateMounts helper copies topology and remaps
root/cwd, constructs a private FsContext, activates membership and replaces the
unpublished child's inherited token/context. Allocation failures leave the
inherited state intact and abandon private prepared storage for deferred cleanup.
This is deliberately not an unshare implementation: its private replacements
are not atomic for concurrent observers. Ordinary context replacement enforces
the task's existing immutable namespace.

Scheduler birth and logical terminal publication now retire a task's token
exactly once, including stillborn threads. Protected kernel operations retain
membership while termination is pending; the final operation boundary publishes
termination and retires membership. Retirement under ready/registry locks does
only the atomic member transition and deferred-queue insertion with wake=false.
Storage release and cleanup wake happen after those locks are released. Final
Process cleanup is an idempotent fallback for unpublished construction unwind,
not the lifetime event that keeps a zombie namespace published.

The actual SchedulerAddProcess, SchedulerKernelOperationBoundary and
SchedulerMarkTerminated bodies are included from small production .inc files.
The VFS host fixture compiles those same bodies against real Process/FsContext
and namespace code, with inert IRQ/policy/registry adapters. It covers stillborn
publication, protected-region and operation deferral, repeated termination,
last-member retirement while observers retain contexts/snapshots, and final
cleanup. Allocation/free and cleanup wake assert that no scheduler fixture lock
is held. This establishes the production decision/ownership paths, not real IRQ
interleaving or arbitrary scheduler correctness. Astra reviewed both integration
and fixture and found no new blocker. The Process tail guard permits alignment
padding, so a future small field inserted into that padding needs manual review.

All 97 host tests pass in one run without competing builds. Affected VFS, real
Ext2/FAT and VMM cleanup tests pass ASan/UBSan, including the actual scheduler
membership fixture. ABI-40 kernel and all 13 matched modules build; compiled
entry/start audits and mutation checks pass. Frozen kernel SHA:
`9d024f9c5d46e427593a8707af68682439c251155e1c78ed3a6290f522cc5d42`.
Evidence is under `build/workloads/mount-namespace-20261022/process`.

Native process-membership regression VM `dc5fd39cc0784e48b296e2160440cfc8`
completed all nine existing filesystem/credentials/IPC jobs with zero live job
tasks. Mousepad launched through the normal UI, rendered a completed frame and
closed with its exact client group gone. Heap integrity passed. Checked sync
and shutdown completed; the original wrapper exited zero and all four offline
filesystem checks exited zero. These exercises cover real initial-namespace
birth/inheritance/termination, not a successful user-facing private namespace.
The checked review is
`build/reviews/mount-process-off-gpu-1ms-fixed-mlfq-a-20261022`.

This is still an intermediate checkpoint within the active goal. CLONE_NEWNS
continues to fail honestly; privileged bind/detach syscall dispatch, initial
arbitrary-bind legacy dispatch, namespace-aware root enumeration and proc mount
reporting, and native namespace isolation/exec/teardown acceptance remain.


## Privileged namespace syscall checkpoint (ABI 40)

The earlier “still incomplete” notes describe intermediate commits. The final
slice adds privileged fork-style `clone(CLONE_NEWNS | SIGCHLD)` and equivalent
`clone3`, directory `mount(..., MS_BIND, ...)`, and subtree
`umount2(..., MNT_DETACH)`. Initial administrative authority is numeric euid 0;
namespace creation grants no privilege. Child root/cwd and topology are copied
before scheduler publication. Exec preserves membership, and logical task exit
retires publication independently of storage retained by zombies, snapshots or
open objects. The foundation is committed in `9a7e32ea` and `21f89df2`.

NEWNS accepts only the fork-style flags above. Shared FS/VM, threads, vfork,
TLS/TID/pidfd flags, CLEAR_SIGHAND and other namespace domains are rejected.
The supported clone3 path validates extension bytes, exit signal, unsupported
fields and stack arithmetic. Mount dispatch accepts exactly MS_BIND or
MNT_DETACH respectively. Recursive binds, remount, propagation, arbitrary file
binds, replacing/detaching the namespace root, virtual source binds, unshare,
pivot_root and tmpfs are outside this slice. Unsupported requests fail rather
than succeeding without their requested semantics.

Initial-namespace legacy driver callbacks project a bind's suffix onto its
retained physical source, including after supported source rename. Directory
enumeration uses the vnode's exact attachment/object to include direct synthetic
mount children without duplicating physical mountpoints. `/proc/mounts` snapshots
the caller's namespace storage; each metadata copy is safe, but a concurrent
mutation can make the whole count/index report non-atomic. Name-based synthetic
brokers are suppressed below a bind so they cannot bypass its topology. This is
not a replacement of every broker with an identity-backed filesystem. The
unbound-root fast path needs separate review before future chroot/root-replacement
support. Backing file data remains shared; namespace copying is not a snapshot.

### Completion audit and limits

- Ownership, lookup authority and teardown: process-owned namespace references,
  independent member tokens, retained backing/attachment paths and deferred
  destruction replace the global eight-slot authority. The initial namespace
  still serves legacy kernel/driver callers. Host tests exercise final-release
  races and actual scheduler birth/termination bodies with lock assertions.
- Failure rollback: allocation injection covers prepared topology, retained
  rename and context replacement; the child's inherited state survives failed
  isolation. Unsupported ancestry fails before backend rename. Existing Ext2
  OOM-to-ENOENT propagation remains explicitly backlogged.
- Isolation and object semantics: the comprehensive differential probe covers
  parent/sibling visibility, ordinary fork and exec, both NEWNS clone forms,
  foreign covered cwd/dirfds, nested detach, root dotdot clamping, bind masking
  of `/proc/version`, parallel mutation and owner exit while a follower retains
  foreign objects. Overlapping workers do not prove specific syscall
  interleavings. These are retained-reference semantics, not a claim that a
  preexisting foreign dirfd is confined to the caller's new topology.
- Privilege/argument behavior: native root tests include setuid denial; a
  separate UID-1000 job checks NEWNS, bind and detach EPERM. Linux comparison
  uses a single-UID user-namespace fixture only to acquire mount authority;
  its privilege-denial case runs separately outside that fixture.
- Component/regression acceptance: all 97 host tests pass together. Affected
  VFS, Ext2, FAT and VMM tests pass ASan/UBSan. Kernel and all 13 ABI-40 modules
  build; compiled entry and scheduler-start audits plus mutation checks pass.
  The same frozen kernel passes nine native filesystem/credentials/IPC jobs,
  normal Mousepad UI launch/render/close and heap integrity. All tracked jobs
  finish with zero live tasks. The original native wrapper exits zero after
  checked sync/shutdown, and all four offline filesystems check clean.

Frozen kernel SHA256:
`351d0fe314dd7a515073955cf99c5a2b26657b14db06a0b7f41e18f38809407c`.
Primary evidence is under `build/workloads/mount-namespace-20261022/dispatch`
and `build/reviews/mount-dispatch-off-gpu-1ms-fixed-mlfq-a-20261022`.
The comprehensive v1 native probe passed. Review then tightened the probe to
require successful mount-report EOF and assert the previously printed Linux
foreign-dirfd/detach observations. Exact v2 passes Linux, including a separate
unprivileged service; both original services are inactive with MainPID 0 and
success. Linux shutdown and offline fsck pass. Final v2 native acceptance passes on fresh checked copies and the identical
frozen kernel. Session `df37e0cbb4b44f6f8d2df12881bf5535` completes both root and
UID-1000 jobs with zero live tasks; heap integrity, checked shutdown, original
wrapper exit zero and all four offline filesystem checks pass. Its review is
`build/reviews/mount-acceptance-off-gpu-1ms-fixed-mlfq-a-20261022`; exact probe
binary/source hashes are in `dispatch/comprehensive-probe-v2-manifest.json`.
The nine regression jobs and Mousepad acceptance were on the first dispatch
review with the identical kernel; only the user-space probe changed afterward. Probe counters are cumulative/inherited, not independent
checks to sum. Astra found no remaining source-review blocker; the final run and disk checks
satisfy its remaining acceptance conditions.

### Recommended next phases and estimate

This removes a substantial VFS ownership prerequisite but does not make
pressure-vessel usable. The next bounded goal should be user-namespace ownership,
UID/GID mapping and scoped privilege checks, with Linux differential tests for
mapping restrictions, inheritance, credential transitions and last-reference
cleanup. Keep unprivileged mount authority unavailable until those invariants
are implemented and tested together. This is another architectural phase of
several focused implementation/test iterations, not a few syscall stubs.

A subsequent mount/runtime phase should use the retained launcher trace to
choose recursive bind, propagation and root-transition operations, implement
only required semantics, and first run an unchanged bubblewrap trivial child.
Then rerun pressure-vessel to discover the next genuine dependency before
promising a Steam QR login. Plan for at least these two further bounded phases;
exact elapsed time or a login date is not supported by the current evidence.
PID/network namespaces, seccomp, full capability compatibility and multilib
remain separate unless the real launch trace establishes them as mandatory.
