# SteamRT feasibility result — 22 October 2026

The bounded investigation confirms that native64 Steam's runtime setup needs a
real VFS and credential architecture phase. It is not a small group of syscall
stubs. No Brook kernel behavior changes in this phase. The recommendation is to
start with retained paths and mount ownership, then add the smallest genuinely
isolated user/mount namespace path exercised here.

## Reference and acceptance

The private comparison runs Linux 6.18.26 under QEMU 10.2.2 with eight vCPUs,
six GiB, Weston 15.0.0 and a guest-only display. SSH is key-only over loopback;
there is no host desktop connection or shared host store. Audio output is none.
The same 27 official packages from the existing native64 manifest are rechecked
by size/SHA-256 and re-extracted. Only archive separators and executable modes
are normalized; original ELF interpreters and launch scripts are retained.

Versions actually queried inside the guest:

| Component | Version |
| --- | --- |
| Steam package manifest | 1792618285 |
| SteamRT depot/platform | 3c.0.20260824.257593 |
| pressure-vessel | 0.20260824.0 |
| Bundled srt-bwrap | bubblewrap 0.12.0 |
| strace | 7.0 |
| Outer NixOS FHS bubblewrap | 0.11.1 |

Steam runs as brook, UID 1000, with private writable profile directories and no
host Steam data. The retained diagnostic launch flags disable bootstrap update/
verification and CEF sandbox/GPU acceleration; this is not default-sandbox or
accelerated-client acceptance. No account name, password or login action occurs.

The NixOS steam-run wrapper supplies the conventional filesystem/library layout
required by the original ELF files. Its own setup is outside the trace. This is
a material comparison limit: the subsequent unchanged SteamRT launcher sees
that filesystem, environment and existing outer user/mount namespace. Nix-store
library exports account for many individual binds; their count is not a universal
minimum for Brook or a conventional distribution.

Three sequential, identity-bound guest services in the first VM are retained:

1. Full selected-syscall trace A captures runtime creation, then CEF repeatedly
   traps. It does not reach a login screen. It is stopped and its group exits.
2. An untraced control reaches the real account/QR login screen with the same
   payload and flags. It is stopped before the next launch.
3. Trace B uses strace's normal-parentage mode and explicitly detaches the exact
   tracer via pidfd/SIGINT at the first steamwebhelper script exec. The unchanged
   application continues to the real login screen. This captures runtime setup
   followed by untraced CEF/UI execution, not an uninterrupted trace through UI.

The tracing-related failure is not attributed to a specific CEF mechanism.
Warm generated state and parentage also differ between these controls. The
successful detached run does establish that the captured runtime setup leads
to the pre-authentication UI without replacing security checks or container code.

A final trace C in a clone of that checked disk captures **all syscalls** during
setup and detaches at the same CEF-script boundary. It also reaches login and
closes the newer-mount-API coverage gap in A/B. All nine checked native client/
runtime executables and scripts still match the original payload hashes. C's
service later reaches its explicit 240-second limit while source analysis is
underway: its recorded result is timeout, not a normal client exit. Its completed
trace is recovered read-only from the stopped, checked disk. No replacement run
or timeout increase is used to turn that receipt into a pass.

All three trace archives, screenshots and launch identities remain private under
`build/workloads/steamrt-feasibility-20261022`. Both original VM owners exit zero
without timeout; serial powerdown and read-only ext4 checks pass. The first VM's
traced/control services stop with success. C's service is already terminal with
main PID zero and an empty control group before the second VM shuts down; its
service-timeout limitation remains explicit.

## Observed dependency table

"Required path" means successfully exercised by this unchanged configuration,
not a counterfactual proof that no alternative launch configuration could work.
References below identify lines in the retained raw trace B.

| Interface or behavior | Observed evidence | Classification and implementation implication |
| --- | --- | --- |
| clone with CLONE_NEWNS\|CLONE_NEWUSER\|SIGCHLD | Five successful clones: four small checks and actual runtime child 2683 | Required path; child isolation must exist before publication |
| UID/GID maps and setgroups policy | Child writes `1000 1000 1`, then `deny`, then `100 100 1` to its proc entries | Required path; map the actual caller's IDs, not namespace UID 0 or hardcoded desktop-wide privilege |
| no_new_privs | Five successful PR_SET_NO_NEW_PRIVS calls | Required path; irreversible inherited state with real privilege-acquisition enforcement |
| Capability handling | capget/capset v3, bounding-set drops, final empty effective/permitted/inheritable sets | Required path; capabilities belong to a user namespace even while numeric UID stays 1000 |
| Root and procfs traversal | `/proc/self/root` opened as a real directory; relative proc dirfds and repeated mountinfo reads | Required path; root is an object and mountinfo must describe the actual topology |
| Private mount construction | Actual child makes 316 mount calls; four check children make ten each | Required path; dynamic topology/backing ownership, not the current eight-slot global table |
| Mount operation classes | Recursive bind, tmpfs, read-only/nosuid/nodev/noexec bind remounts, recursive slave/private propagation | Required path; flags need effects on lookup and access, not successful no-ops |
| Root replacement and detach | `pivot_root("/tmp", "oldroot")`, later `pivot_root(".", ".")`; two MNT_DETACH unmounts | Required path; retained old roots and open objects survive appropriate detach without remaining reachable through ordinary new-root traversal |
| Files, directories and socket bind mounts | Library files, home directories, X11/Wayland/D-Bus/audio socket paths and generated metadata files | Required path; mount sources cannot be limited to directory/device mounts |
| Child lifetime | PR_SET_PDEATHSIG and PR_SET_CHILD_SUBREAPER succeed | Required path; real parent-death and adopted-child/reap semantics need separate acceptance |
| Named FIFO | mknodat S_IFIFO\|0600 succeeds; re-exec receives EEXIST | Remaining Brook IPC gap; pathname lifetime and FIFO open/read/write/wakeup semantics required |
| Hardlinks | B has 6,608 successful linkat calls and 92 successful link calls | Used optimization with source-confirmed copying fallbacks; fallback compatibility/performance still needs Brook acceptance, not fake success |
| i386 capture helper | B executes i386-linux-gnu-capsule-capture-libs and makes 32-bit syscalls | Architecture probe with a source-confirmed skip path for default/directory providers; not evidence that IA32 is a login prerequisite |
| PR_SET_MM_ARG_START | EPERM ten times, followed by successful setup/UI | Observed tolerated failure; preserve a real error rather than implementing success for this phase |
| PID/network namespaces | No such flags in captured clones; live outside/inside processes retain the same PID/net namespace IDs | Not a demonstrated requirement of this configuration; do not add to the initial implementation scope |
| unshare/setns/chroot/seccomp | Selected explicitly, no calls in captured setup | Not exercised; no support claim. CEF sandbox was disabled and post-detach calls are outside B's coverage |
| New mount APIs | Unfiltered C makes no fsopen/fsconfig/fsmount/open_tree/move_mount/mount_setattr calls before detach | Not demonstrated setup requirements; no claim about later execution or other configurations |

The main mapping writes are `calls.2683`; the root open is `calls.2650:320`,
and subreaper setup is `calls.2683:10133`. Full argument lists and per-call
file/line references are in private `dependency-events-{a,b,c}.json` and
`dependency-summary-{a,b,c}.json`. The earlier cold trace has 312 actual-child
mounts versus B's 316 and C's 314; this is state-sensitive, not a benchmark.

Selection in A/B includes process/file/credential/network calls plus explicit legacy
namespace, mount, privilege, descriptor and data-I/O operations. It is not a
trace of every syscall; C supplies that coverage through its detach boundary.
Successful calls alone do not establish which optional checks can
safely fail. No unsupported fallback is invented from missing trace lines.

### Source-confirmed fallbacks

The matching [Valve source archive](https://repo.steampowered.com/pressure-vessel/snapshots/0.20260824.0/sources/steam-runtime-tools_0.20260824.0.tar.xz)
has SHA-256 `18c3d0676445b61a811cacaf68beefce0b7ca92de2dbf909c5948e5a72d3edc1`.
In pressure-vessel/runtime.c:793–809 and 8252–8255, inability to execute an
architecture's capture helper causes a warning and architecture skip for default/
directory providers. At least one working architecture is required. Explicit
provider JSON metadata bypasses that probe and can make later capture failure
fatal. The captured wrap arguments/environment supply no explicit provider
manifest. This supports trying the existing x86-64-only Brook path; it does not
prove its fallback or 32-bit game acceptance.

pressure-vessel/mtree.c:638 onward falls back from failed linkat to a real copy;
tree-copy.c:174–205 likewise copies after failed link, without restricting errno.
Those branches trade time/space for compatibility. Neither is exercised by the
successful hardlinks in B. Test copying, metadata and failure cleanup in Brook;
do not manufacture successful links or EXDEV to steer the program. Proper
hardlink support remains valuable separately, including normal Xwayland startup.

## Minimum object and lifetime design

Astra reviewed the current implementation before the trace. Its constraints
match the observed workload: cwd is currently a string, mount dispatch is global,
and many permission checks use numeric UID 0 as global privilege. The proposed
minimum design is:

| Object | Ownership and contract |
| --- | --- |
| Retained path | Mount attachment reference plus stable identity for any mountable object, including files and sockets; root/cwd additionally require directories |
| Filesystem context | Refcounted effective root, cwd and umask; clone sharing versus copying is explicit |
| Mount namespace | Refcounted topology and owning user namespace; copies duplicate attachments, not filesystems/devices |
| Attachment and backing | Separate namespace attachment from filesystem instance; namespace detach cannot unmount backing still owned by another attachment or open object |
| User namespace | Parent reference, stable owner identity and validated UID/GID maps with setgroups policy; no raw creator Process pointer |
| Credentials | Stable kernel IDs, explicit namespace translation and scoped capability state; namespace-local authority cannot become initial-namespace root |
| Privilege ceiling | Per-task irreversible no_new_privs, copied/preserved through clone/exec and enforced at supported privilege-acquisition paths |

Path lookup must receive the namespace and retained root/start objects. Absolute
symlinks restart at the effective root; `..` crosses mount boundaries correctly.
Directory descriptors retain identity rather than being reconstructed from an
old pathname. Proc root/fd links require brokered access to real referenced
objects with credential checks. Preserve legitimate inherited outside-dirfd
behavior: chroot alone does not revoke those capabilities. The launcher must
close/restrict them, and tests must distinguish that from accidental lookup escape.

ID translation must be consistent across file ownership, process inspection,
SysV permissions, Unix credentials and executable authorization. Do not encode
namespace root by changing Process::euid to zero and reusing global-root checks.
The observed single-ID maps can bound a first implementation, provided unsupported
mapping forms fail honestly and the map/parent/permission model remains real.

Publication and teardown rules:

- Snapshot/retain process context under a short publication lock, then release
  it before filesystem work. Use a sleeping per-namespace mutation mutex and
  immutable, refcounted topology snapshots; scheduler/IRQ locks never span
  backend callbacks.
- Build a complete clone/unshare/root transition privately. Retain backing and
  path references before atomic publication. On failure unwind in reverse order,
  leaving parent/desktop topology and credentials unchanged.
- Release replaced references outside publication locks. Final backend teardown
  runs in deferred cleanup, not SchedulerMarkTerminated with interrupts disabled.
- Define attachment/path ownership so topology references cannot create immortal
  cycles. Detach removes reachability; final reference release destroys backing
  only when all attachments and open objects permit it.

The proposed lock protocol separates process publication, filesystem-context
snapshotting and topology snapshotting: retain each object, unlock, then enter
the next domain. None of those short locks nests with a backend lock or the
namespace mutation mutex. Mutators take that sleeping mutex before invoking
backend/inode operations; backend code must not acquire namespace mutation
locks in reverse. A short topology-publication lock swaps a prepared snapshot
without callbacks or destruction. Process root/credential changes compare the
expected context generation before publication and unwind a stale attempt.
Cross-namespace transactions and filesystem-specific inode lock ordering need
explicit review before implementation; this protocol is not a concurrency proof.

Astra's final review finds no material blocker to completing this bounded
investigation. It confirms the proposed lock protocol is consistent, provided
references are retained before unlocking, callbacks/destructors never reacquire
the namespace mutation mutex, and stale-generation unwind happens outside
publication locks. Detailed backend identity and inode-order review remain
implementation gates, not work claimed complete here.

Process-owned context pointers are preferable to additional PID/incarnation side
tables for pervasive lifetime-bearing state. Process/Vnode/filesystem/module
layout changes need a module ABI audit and synchronized rebuild; current ABI is
34, and appending fields is not automatically compatible. Initial-namespace APIs
may remain as kernel-driver wrappers, while user lookup carries explicit context.

## Recommendation and bounded implementation estimate

This is an architecture effort measured in weeks of focused engineering and
review, not an overnight compatibility patch. Treat 15–30 engineering days as an
unvalidated planning allowance, not an evidence-backed delivery schedule. Hardlink
fallbacks, helper architecture and filesystem integration can exceed it. A useful
first limit is a two-to-three-engineering-day design/prototype slice, followed by
review and re-estimation even if its acceptance is incomplete. These are planning
bounds, not measured build times or promises of agent wall-clock duration.

1. **First separately accepted slice: retained paths and filesystem contexts.**
   Preserve current desktop behavior; prove rename/dirfd/root lifetime and failure
   unwind before adding namespace creation. This is useful infrastructure but
   does not itself constitute Steam or isolation acceptance.
   The [first implementation checkpoint](retained-filesystem-context.md) fixes
   vnode/mount lifetime and records why a backend-relative walker is required
   before process-context migration; the full slice is still unfinished.
2. **Mount topology and scoped credentials.** Separate attachment/backing ownership,
   provide genuine namespace creation and validated single-ID maps, implement
   the observed propagation/bind/tmpfs/remount/pivot/detach operations and real
   privilege drops. Review lock/lifetime design before integration.
3. **Launcher acceptance.** Add coherent procfs views, necessary parent/subreaper
   semantics and FIFO support; validate a trivial unchanged srt-bwrap command,
   then unchanged pressure-vessel, then native64 Steam as brook to login only.

Each slice needs Linux differential probes and native Brook tests for invalid
arguments, permissions, parallel operations, fork/exec, final-reference teardown
and allocation-failure rollback. Add isolation-negative tests for namespace
cross-talk, symlink/.. traversal, retained paths and privilege translation.
Retain current IPC and filesystem acceptance. Stop after the first slice for a
review of cost and risk before committing to the full remaining implementation.

Do not add PID/network namespaces, CEF sandbox support, game launching or GPU/
scheduler tuning on the strength of this trace. The [original plan](steam-runtime-container-plan.md)
remains the implementation gate; authorization for this investigation does not
authorize that later implementation.
