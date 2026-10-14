# Surviving branch audit — 14 October 2026

Follow-up investigations are now tracked in [backlog.md](backlog.md), deferred
while VM-only MCP control is the active development focus.

Remote refs were fetched before this review. `origin/main` was `76c3155`;
the tested recovery base was `origin/reconcile/gl-stack-to-main` at `e60f80c`.
That reconciliation preserves the hardware virgl stack, GLtron/STK launch
tools, mip-level resource creation, and earlier filesystem/locking fixes.
It is included in the recovery push to main.

The July fixes `06a8d07`, `b2ae028`, and `e896db0` were absent from the
surviving refs. Their GL3 object-limit, draw-aware splitting, and present
barrier logic was reconstructed from saved observations and tested locally.
These are new patches, not recovered original commits. See
[experiments.md](experiments.md) for validation and remaining rendering issues.

## Work worth preserving separately

The following are candidates for review and porting, not assertions that
their old patches are ready to merge into the current kernel.

| Area | Surviving commits/branch | Assessment |
| --- | --- | --- |
| Ext2 rename replacement | `d9bbaf5` on `diag/bro208-switch-txn` | High priority: frees replaced destination inode/blocks. Review same-inode rename and open-file lifetime before porting; does not establish a fix for our observed directory corruption. |
| Wayland keyboard and SDL text input | `92bb917`, `c455b28`; `0076f9d` on `cherry-pick/stable-inodes-valuable` | High priority: keyboard focus/modifier order and text-input protocol support are relevant to our current console/input problems. |
| Fullscreen and opacity after resize | `34dee1a`, `2791c87` on the diagnostic branch | Useful app behavior; port with compositor/window ABI review and resize tests. |
| Crash capture and profiler | `5c5bbff`, `98caab7`, `7818694`, `d2d5f00`, `f47c931`, `0c551ae`, `a2cadd7`, `63d3b63` | Valuable fault-safe unwinding, panic log capture, readable profiler symbols, and write-failure reporting. Some require preceding kernel changes. |
| mmap read-ahead | `13783b3` on the diagnostic branch | Relevant to Quake loading performance; saved commit claims a large page-fault reduction, not reproduced in this recovery. |
| Input probes and API-gap harness | `tools/sdl2-input-probe`, `tools/wayland-gap-audit`, `tools/p2-gate` on the diagnostic branch | Useful diagnostic assets, including HMP and serial helpers for future VM-control MCP work. |
| Nix installer closure handling | `211dd34`, `fe75fe2`, `820b42a` on the two `fix/nix-install-*` branches | Full dependency verification/repair and transitive library linking. Preserve source, rebuild binaries; the old commits include generated executables. |
| Window/input/shell polish | `ec1c543`, `9658913` on `cherry-pick/stable-inodes-valuable` | Alt+Tab/cursor fixes and quiet launch mode may be useful. The branch's shift-state change was explicitly reverted; do not resurrect that reverted patch. |

The diagnostic branch has 69 patch-distinct commits relative to the recovery
base, plus several changes already represented by other patches. It also
replaces/removes substantial parts of the recovered graphics stack. Do not
merge its entire tip. Keep `diag/bro208-switch-txn` for selective recovery.
In particular, `6a1d563` identifies itself as WIP and unverified against the
baseline; scheduler ownership/context-switch experiments need their own
validation. `8c75b03` changes AP startup-stack ownership and also needs review.

## Other distinct historical branches

`diag/hang-dump-block-reasons` and `fix/bro176-aslive-incarnation` contain
additional diagnostic instrumentation. `feature/i2c-driver` contains an
Intel i801 SMBus polling implementation, useful if hardware support is
resumed. `feature/demand-paging` and `feature/process-lifecycle` contain old
patches whose behavior must be compared with the later memory/process code
before any port. `feat/screenspace-alpha-compositor` and
`feat/waterline-chrome` are visual experiments; retain as references rather
than replace the tested compositor.

The old `feat/yquake2-gl` launch/prestage branch is mostly superseded by our
new pinned experiment profile and isolated disk runner; its later hang/futex
diagnostics are also represented on `diag/bro208-switch-txn`. The useful
OpenAL OSS and yquake2 package patches from that diagnostic lineage have
already been preserved in this recovery.

Remaining remote branches had no patch-distinct commits beyond the tested
recovery base under `git cherry`. This compares patches, not just merge
ancestry; it does not certify every historic experiment's behavior. No
remote branches were deleted during the audit.
