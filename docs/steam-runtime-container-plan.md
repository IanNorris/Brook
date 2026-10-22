# SteamRT container feasibility gate

Status: proposed scope expansion, not an implementation or login acceptance.
The current Steam goal explicitly excludes full runtime-container support.
The tested IPC checkpoint is commit `bf453666`; [the client notes](steam-client.md)
record its native acceptance and private VM evidence.

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

## Current implementation boundaries

These are source observations, not an exhaustive trace of Steam's requirements.

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
