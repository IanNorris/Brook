# Signal delivery and desktop follow-up — 2026-10-18

After the timerfd repair, starting STK alongside both browsers exhausted physical
memory in the native review VM. The kernel then faulted while clearing a signal
frame directly on an unbacked user alternate stack. This occurred on RR; MLFQ
was registered but inactive. The original panic disks remain unmodified forensic
copies and were not used for acceptance.

Signal delivery now builds its frame on the kernel stack, checks placement and
alignment, and copies through the fault-safe usercopy path. Demand paging and COW
may allocate or sleep during that copy. The signal mask, alternate-stack state
and return registers are committed only after a successful copy. An invalid or
inaccessible frame terminates the affected thread group with SIGSEGV instead of
recursively faulting in the kernel. This covers synchronous exception delivery
and pending signals at syscall return. It does not implement an OOM victim policy.

The CPU compositor now restores the affected wallpaper region and redraws all
intersecting layers in order before blending translucent content. Damage expands
through overlapping layers, so clean upper windows also receive a rebuilt
backdrop. Native framebuffer blits are clipped to the current client dimensions.
The GPU compositor path is unchanged. Conservatively merged damage may repaint
a larger area; mixed-window and 4K performance still need measurement.

Desktop overlays now consume pointer events before raw-grabber and per-window
routing, retaining button ownership through release even when the launcher
closes on press. This addresses launcher click-through; it is not a full client
pointer-grab or drag implementation.

## Acceptance

The tested kernel SHA-256 is
`ee073c45e936abddfb101b25b5b407150989ae6334fd6f0da52f59242cc70e00`
(module ABI 18). Acceptance used eight CPUs, 4 GiB, software composition and fresh
private copies of the last checked timerfd disks. Host audio output was disabled;
the guest PCM buffer remained available for measurement.

- Release build, supported entry-template audit and all 40 native suites passed.
  Host tests include frame-layout bounds/alignment and a 1,000-frame partial
  alpha-composition oracle, overlap closure, blit clipping and pointer ownership.
- The owned `scripts/workloads/signal_delivery_check.c` fixture passed 18 guest
  cases as UID 1000: pending SIGUSR1 and synchronous SIGILL, ordinary/lazy/COW
  stacks, unmapped/read-only/PROT_NONE stacks and a protected-page crossing.
  Valid handlers returned; inaccessible stacks terminated only the child.
- Two additional physical-exhaustion runs covered lazy and inherited COW
  alternate stacks. Both reached `free_pages=0` and logged an inaccessible fault
  frame, terminated the child with SIGSEGV, and left the parent, kernel and heap
  check working. These are real allocation failures, not injected errors.
  Pressure modes belong only in a disposable, memory-bounded VM.
- Firefox played the owned 24 fps fixture after the first pressure run. Ten
  seconds of captured PCM contained 441,230 frames at 44.1 kHz with no silent
  10 ms bins. An 18.45-second browser sample measured 0.994 media seconds per page
  second and 21.96 video callbacks/s. This is a smoke check, not a matched
  performance improvement claim; 26 reported dropped frames remain recorded.
- Ten unedited 1920×1080 screenshots over 22.34 host seconds showed identical
  top/left/right shadow strips while all ten video-region hashes differed.
  Captures briefly pause QEMU and were separate from PCM measurement.
- A browser DOM event counter received neither the press nor release of launcher
  dismissal clicks outside the menu or on its header. Ordinary control clicks
  before and after each dismissal arrived. Actual app-entry selection and all
  pointer-grab combinations are not established by this check.
- All five tracked guest commands exited zero, with complete stdout/stderr and
  drained original and launch lineages. Firefox's full launch lineage drained
  on native close. Audio was inactive afterward, heap integrity passed, checked
  sync completed, QEMU and its launcher exited zero, and read-only FAT/ext2
  checks returned zero for boot/data/Nix. Five saved documents retained their
  exact hashes and UID 1000:GID 100 ownership.

Intentional user faults are scoped to the stack/OOM probes. The browser/UI
interval contained no user fault, and the full serial audit found no kernel
panic or device fault. Evidence is retained locally under
`build/workloads/signal-frame-delivery-20261018/acceptance-audit.json` and `review/`.
The complete bootstrap pair was accepted on the earlier pinned revision; it was
not rerun for this follow-up.

Concurrent-browser contention, intermittent real-site audio stalls, broader
move/resize/occlusion checks and a user-visible memory-pressure policy remain
open. [MLFQ safety](mlfq-safety.md) records separate integration blockers; the
scheduler remains RR.
