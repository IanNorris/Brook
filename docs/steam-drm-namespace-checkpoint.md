# SteamRT retained DRM visibility checkpoint

Steam login is not yet accepted. This checkpoint fixes an independently reproduced
GPU discovery failure while preserving the remaining Steam startup failure.

## Before and after

Before this change, a controlled child launched through the same SteamRT helper
could not stat or open `/dev/dri/renderD128` (ENOENT). ANGLE Vulkan initialization
worked outside the container but failed within it. Genuine directory binds hid
the synthetic DRM device because the general broker deliberately rejects paths
beneath overmounts.

The kernel now pins the boot `/dev/dri` and `/sys/dev/char` objects. Only known DRM
broker paths can pass through a genuine bind of those objects. Unrelated parents,
real shadow entries, symlinks and reserved mount subtrees deny broker visibility.
Metadata inquiries do not activate devices: new opens retain the deepest `nodev`
check. Existing open descriptors remain usable, as on Linux. `statx` now reports
character-device major/minor numbers for paths and existing descriptors.

Both parent directories are packaged in fresh initrds. Runtime mkdir on the FAT
root is unsupported, so attempting to create them during boot was a failed
candidate. FAT also rejects the fixed virtual `226:128` component with EINVAL.
The bridge treats that component as absent only beneath its pinned parent and
after excluding reserved overmounts; arbitrary paths receive no such fallback.

## Validation

Candidate v4 ELF SHA256:
`4cd01bdbf7ffe4dc1487a3d6ecf5eda7a938b9327f798bc9128ab8c520aa791e`.

- All 102 host tests pass with assertions enabled. The parent Release configure
  initially disabled assertions and caused unused-variable build errors; the
  preserved failed build was followed by an explicit Debug host configuration.
- Retained-path ASan/UBSan tests pass, including genuine binds, fake parents and
  children, namespace copies, readonly mounts and `nodev` activation checks.
- Exact-ELF entry-state and scheduler-start mutation audits pass.
- Native disposable-guest DRM probe passes: 99 fixture checks and 36 parent
  checks, covering user/mount namespaces, copied namespaces, narrow/full device
  binds, fake devices, retained descriptors, nodev, pivot-root and old-root detach.
- Native socket, unsupported PI-futex, procfs task, packet-pipe and robust-futex
  probes pass. A 64 KiB tmpfs quota fixture and PulseAudio write fallback pass.
- Inside SteamRT, the controlled probe enumerates renderD128, opens it, obtains
  DRM version 0.1.0, and initializes ANGLE Vulkan as EGL 1.5 with EGL_SUCCESS.

Native probe failures from earlier candidates are retained separately: missing
FAT directories caused an early boot panic; virtual sysfs attribute lookup then
failed EINVAL before the fixed-component fallback. Automatic approval review
rejected host execution of the probe because its source contains namespace/mount
fixtures. Execution was confined to disposable guests; no host test was retried.

## Remaining failure

Full Steam still exits 135 in both software and GPU-enabled diagnostics. The
prelaunch shared-memory monitor starts empty. During the software run it observes
nine 25 MiB plus 16-byte pools and approximately 242 MB logical files before the
roughly 527 MB canonical quota is exhausted by backing plus cache allocations.
FILEFAULT reports ENOSPC. This is not evidence that increasing the quota alone
will produce a usable client. A successful Linux reference stabilized at two
such pools and approximately 58 MB logical shared files.

Fixed-message counts show a shared-memory corruption diagnostic before the
allocation failure. A synthetic 25 MiB plus 16-byte unlinked mapping test passes
with writable/readonly aliases, fork, SCM_RIGHTS, and child-to-parent writes.
The first version of this diagnostic compared 16 bytes against a 17-byte constant;
that harness failure is retained and is not attributed to the kernel.

The process-traced Steam attempt leaves an orphaned helper; it was identified by
its exact executable and stopped separately. The guest records one exit-group
timeout, so the final serial stability acceptance fails. The original owned VM
wait returns zero, the heap integrity check passes, and all four offline
filesystem checks return zero. These results do not constitute full Steam or
whole-system stability acceptance. Fixed diagnostic parsing finds GPU child launch code 1002 in the cumulative
CEF log. A subsequent prelaunch baseline already contains all six occurrences,
and a later snapshot still contains six. Those entries therefore do not
establish a current GPU child launch failure and must not be used as a new
regression boundary.

Raw client logs, binaries, profiles, cookies, disk images and request receipts
remain private ignored artifacts. No authenticated session is claimed.


## Focused tracing follow-up

Optional process tracing now records exit/exit_group before dispatch, and records
exec attempts before the nonreturning transition. Previously, the post-call
logger could never report successful exec or exit. The debug command parser also
accepts `other`, exposing the existing remaining-syscall category (credentials,
process controls and other calls outside the six named groups).

A native thread/fork control verifies `strace <pid> proc,other`: its child exits
23, the creating thread's getuid reports 1000, and both thread/process normal
exits appear in the serial trace. The new exact ELF entry-state and scheduler
mutation audits pass. This changes optional diagnostics only; it does not fix
Steam startup. Concurrent syscall trace prefixes and result suffixes can still
interleave, so a naive regex must not assign their error numbers to a call.

The stopped Linux reference image was inspected read-only for fixed-message
counts. None of the sampled relative-path, shared-memory corruption, or GPU
launch-failure messages appear there. Brook's wrapper identifies the relative
field as browser_subprocess_path, while a reduced SteamRT Python child resolves
proc/self/exe and relative realpath successfully. The application-specific
resolution difference remains under investigation. One overlapped SteamRT setup
attempt exited 75; it is retained as a harness/setup failure, not client
acceptance or a new kernel regression. Later tests run constructors sequentially.
