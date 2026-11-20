# PID namespaces

Brook keeps kernel process identifiers stable and gives each PID namespace a
numbering of its own. A task is visible in its active namespace and every
ancestor. A task cannot use namespace-relative numbers to address an ancestor
or sibling. The initial namespace continues to expose the current Brook numbers.

## Identity and publication

- Keep the existing global PID, TGID and process incarnation for scheduler
  lookups, ownership, wait queues and diagnostics. Store namespace-local IDs
  in a separate bounded-depth task identity. Resolve caller-visible numbers
  through the scheduler's protected task registry, returning a pinned process
  identity. Never feed untrusted local numbers into global lookup.
- Assign a number in the child's active namespace and each visible ancestor.
  Reserve numbers before publishing the task and retain them through zombie
  lifetime. The local allocator wraps below 2²² and skips IDs still leased
  as a TID, TGID, process group or session. Match user-visible TGID,
  parent and process-group results using
  the identity visible from the caller. A parent outside the view has PPID 0.
- CLONE_NEWPID creates the child's namespace and its first process is PID 1.
  An unshare operation selects a namespace for future children while the
  caller keeps its current namespace. Keep those two references independent.
  Reject allocation under a dying namespace. Publish a prepared branch of
  child namespaces, credentials and identity together with the child.
- Tie PID 1 lifetime to the init thread group. When it exits, prohibit new
  members before collecting live member references, then terminate and reap
  its descendants. Reparent orphans to an in-namespace subreaper or namespace
  init. Keep the scheduler's existing initial-namespace reparenting behavior.

## Boundaries

The first functional slice must cover fork and clone return values, child
and parent TID writes, getpid/getppid/gettid, process groups and sessions,
wait4/waitid, kill/tkill/tgkill and signal sender identities. Group operations
must filter membership by the caller's namespace before permission checks.
Apply namespace-init signal rules separately from ordinary permissions,
including terminal job-control delivery. In the initial namespace,
`kill(0)`, `kill(-pgrp)` and `kill(-1)` now use Linux-style selection.
In particular, an initial-namespace broadcast may signal every permitted
same-UID user process, including desktop services. It excludes the caller's
thread group, PID 1 and kernel threads.

A proc mount owns the PID view captured at mount time. Enumerated directory
names and numeric lookups use that view. The same view controls self,
thread-self, task directories and the PID, TGID, parent, group and session
fields in status/stat. A retained proc inode continues to use its original
view and target incarnation after a mount move or process-number reuse.

Audit remaining interfaces before carrying the clone flag outside the
fixture. This includes process references from ptrace or pidfds where
implemented, process-directed timers, asynchronous signal metadata, PI
futex owner words, robust futex cleanup, and SCM_CREDENTIALS/SO_PEERCRED.
Record signal senders and socket peers with internal identities and
translate for the recipient. Keep the original task's namespace alive
where shared user data contains its visible TID, and apply explicit
ownership checks before following that number. An unsupported interface
must fail explicitly if a namespace-safe interpretation is unavailable.

## Implementation boundaries

Brook accepts fork-style PID namespace creation through clone and clone3,
with SIGCHLD and compatible namespace flags. Standalone unshare(CLONE_NEWPID)
selects the namespace for future children. Joining arbitrary PID namespaces
through setns and PID file descriptors remains unsupported.

The active PID namespace must have a live init group before another member
can start. When the last init thread exits, it closes admission and the
scheduler kills descendants. The ancestor's wait remains pending until
descendant task records have retired their resources. An in-namespace
subreaper or init adopts orphans while that namespace is still running.

Fresh proc mounts require authority over the PID namespace's user owner
and an existing complete, reachable proc mount with a compatible view.
The new mount inherits the source's restrictions. This permits an inherited
ancestor view to authorize a narrower mount and preserves existing masks
and read-only attachments.

Robust futex owner words use the task's local TID. Shared PI futex waiters
must use the same active PID namespace because the owner word has one
numeric representation. Exec from a nonleader thread inside a private PID
namespace currently returns EOPNOTSUPP. It requires preserving Linux's
thread-group leader identity across exec.

## Validation

- Compare parent, nested and sibling namespaces for self, parent, thread,
  session and process-group IDs. Check wait/kill permissions, pidfd and
  proc misses for the wrong namespace or a reused number. Exercise fork
  failures with no leaked process, number or namespace. Cycle through the
  full numeric range while retaining group numbers.
- Verify init's signal rules, orphan reparenting, child reaping, group
  teardown and refusal to fork into a dead namespace. Retain a proc mount
  after namespace teardown and verify it cannot resolve another namespace's
  tasks. Exercise nested proc, socket credentials, and concurrent exits.
- Test the unmodified bubblewrap `--unshare-all --ro-bind / / --proc /proc
  --dev /dev --tmpfs /tmp` ladder under headless QEMU with K1.
  Run the Flatpak-style option and filter fixtures with K3 and check both
  initial and managed-profile guarded boots on the integrated image.

[Linux namespace ABI](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html)
· [Linux implementation](https://github.com/torvalds/linux/blob/v6.12/kernel/pid_namespace.c)
