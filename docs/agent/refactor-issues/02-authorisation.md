# Brook syscall authorisation (needs Ian)

_6 open entries (3 high, 3 low). Part of the split refactor issue log; index in [../REFACTOR_ISSUES.md](../REFACTOR_ISSUES.md). Fixed entries are in [done.md](done.md)._

- **Scope:** Which processes may call the Brook-specific syscalls that expose input, window pixels and the desktop: legacy compositor calls 521-524, the input grabber, `DESKTOP_LAYERS`/`DESKTOP_OVERLAY`/`DESKTOP_CLIENT_FRAME`, cursor image, profiler and crash-entry calls, `query_source_commit`.
- **Why it matters:** Any process can currently grab all input, capture the desktop or spoof the screen. It is a policy decision, so nothing here can be fixed until Ian picks the model.
- **Needs Ian:** Decisions wanted: (1) adopt the display-server capability as proposed below; (2) whether 521-524 can be retired; (3) whether `debug_frame` stays root-only with its own capture slot.
- **Dependencies:** None to start. H-21 also touches the Vulkan renderer and bridge capture tools (`tools/vulkan-compositor-pkg`), so test on the Dell/QEMU desktop after gating. L-44 shares code with 11 (WM).
- **Suggested order:** Decision first. Then the capability plumbing plus H-03 and H-04, then H-21, then L-43, L-44, L-46 (which reuse the same check).
- **Test approach:** `test_syscall_user_memory/grab.cpp` already tests `InputGrabTakeoverAllowed`; extend it with a fake process carrying/not carrying the capability. Add a QEMU test that an unprivileged process gets -EPERM from 521-524.
- **Size:** Small to medium once decided (6 entries, one branch). Waiting on Ian is the real cost.

### Authorisation proposal (H-03, H-04, H-21; Ian decides)

Current in-tree users:

- waylandd uses the per-window WM syscalls (506-520, 525), and none of 504, 505 or 521-524.
- The Vulkan renderer (`tools/vulkan-compositor-pkg`) uses `DESKTOP_LAYERS` (0xB19) and the overlay (0xB1A).
- The bridge's frame capture goes through the debug command path and `debug_frame`.

Proposal:

1. **Add one display-server capability.** It is a process-wide flag set by the kernel, never by the process itself.
   - It goes to the processes the boot script starts as the desktop: the registered offscreen renderer (`BdwOffscreenRendererOwnedBy`), waylandd, and root.
   - It survives `exec` only into those binaries, and never passes through `fork` into an unrelated `exec`.
2. **Gate these syscalls on it**, returning -EPERM otherwise:
   - `input_pop` (504) and `input_grab` (505);
   - the legacy compositor calls 521-524 (scanout map and flip, window list, mapping any window's VFB);
   - `DESKTOP_LAYERS` (0xB19), and the full-frame ops 0 and 2 of `DESKTOP_OVERLAY` (0xB1A);
   - `DESKTOP_CLIENT_FRAME`;
   - the cursor image set and reset (L-44).
3. **Retire 521-524** if Ian confirms nothing out of tree uses them. There is no in-tree caller.
4. **Keep `debug_frame` root-only**, and give it its own capture slot so that a desktop capture cannot starve it.

Already done, with no policy change: `input_grab` refuses to replace a live grabber unless the caller is that grabber or root (`ea753d08`).

## Entries

### High

### H-03 · Legacy user-mode compositor syscalls 521-524 lack any privilege check

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:608`, in `sys_brook_wm_scanout_map`.
- **Severity:** high (confidence high).
- **Problem:** Any process can map the compositor backbuffer read/write, push rows to MMIO (scanout_flip), list every window (get_windows) and map any window's VFB (map_window_vfb, where WmMapWindowVfb only checks that the caller is non-null). That allows cross-process pixel leaks and screen spoofing. compositor.h says "privileged userspace process", but nothing enforces it. 521 and 524 also skip the ProcessCurrent() null check.
- **Suggested fix:** Gate on a compositor capability (root, the registered input grabber or the display server), or retire the syscalls if unused. Otherwise return -EPERM.
- **Test:** QEMU test: an unprivileged process calling 521-524 gets -EPERM.
- **Status (fix pass):** open: policy decision for Ian; proposal under "Authorisation proposal" below. Confirmed: 521-524 have no in-tree caller.

### H-04 · Any process can become the global input grabber (keylogger, waylandd DoS)

- **Where:** `src/kernel/src/syscall_brook_system.cpp:203`, in `sys_brook_input_grab`.
- **Severity:** high (confidence high).
- **Problem:** CompositorSetInputGrabber replaces the current grabber unconditionally, so any process receives all keyboard and mouse input and the display server stops getting it.
- **Suggested fix:** Require privilege, and refuse while a live grabber is registered unless the caller is that grabber.
- **Test:** QEMU test: non-root 505 gives -EPERM and the existing grabber keeps receiving input.
- **Status (fix pass):** hardened in `ea753d08` (no silent takeover of a live grabber; root may take over). Policy still open, see "Authorisation proposal". Test: host test of `InputGrabTakeoverAllowed` (grab.cpp).

### H-21 · Desktop capture/overlay syscalls expose window pixels and full frames to any process

- **Where:** `src/kernel/src/syscall_brook_desktop.cpp:165`, in `sys_brook_desktop_layers / sys_brook_desktop_overlay`.
- **Severity:** high (confidence medium).
- **Problem:** The only gate is BdwRenderExperimentEnabled(). Any process can export layer pixels, chrome and backdrop, or request and read a full composed frame (overlay op 0/2). That bypasses debug_frame's root-only check and shares its single capture slot (a DoS for root screenshots). BROOK_DESKTOP_CLIENT_FRAME also lets any process call WmNoteClientFrame for any id.
- **Suggested fix:** Require the registered offscreen renderer (BdwOffscreenRendererOwnedBy) or root for every op.
- **Test:** QEMU test with the experiment on: an unrelated process gets -EPERM on 0xB19 and 0xB1A.
- **Status (fix pass):** open: policy decision for Ian; proposal under "Authorisation proposal" below. Not hardened: the Vulkan renderer and bridge capture tools use 0xB19/0xB1A.

### Low

### L-43 · query_source_commit lacks a caller/ownership check; bad pointer returns -EINVAL

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:125`, in `sys_brook_wm_query_source_commit`.
- **Severity:** low (confidence medium).
- **Problem:** This is the only source-commit syscall without proc. Any window can be read by slot, gated only by the incarnation. An unwritable output returns -EINVAL, not -EFAULT. Cross-process access may be intended (scene service).
- **Suggested fix:** Document it as capability-by-incarnation or add a provider check; return -EFAULT for a bad pointer.
- **Test:** Host test: an unwritable output gives -EFAULT.

### L-44 · Cursor image set/reset lacks authorisation; hand-rolled user copy

- **Where:** `src/kernel/src/syscall_brook_wm.cpp:539`, in `sys_brook_wm_set_cursor_image`.
- **Severity:** low (confidence medium).
- **Problem:** Any process can replace or reset the global cursor (the reset runs before the ProcessCurrent check). The hotspot is unbounded. A page-walk memcpy under addressSpace->mutation duplicates what CopyFromUser would do.
- **Suggested fix:** Gate on the cursor owner or grabber, bound the hotspot, and use CopyFromUser.
- **Test:** Host test: an unprivileged caller gets -EPERM; an out-of-image hotspot gives -EINVAL.

### L-46 · Profiler and crash-entry syscalls lack authorisation and atomicity

- **Where:** `src/kernel/src/syscall_brook_system.cpp:57`, in `sys_brook_profile`.
- **Severity:** low (confidence medium).
- **Problem:** Any process can start or stop the system-wide profiler. set_crash_entry's `if (leader->crashEntry != 0) return -EINVAL;` is a non-atomic check-then-set, accepts kernel-half addresses, and returns EINVAL rather than EBUSY. crash_complete is allowed with no crash in progress.
- **Suggested fix:** Root (or a capability) for profile; a CAS on crashEntry; validate it as a user address; gate crash_complete on crashInProgress.
- **Test:** Host test: concurrent set_crash_entry has exactly one success; a kernel address gives -EFAULT.
