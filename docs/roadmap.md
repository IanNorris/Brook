# Brook roadmap: 14 October–14 November 2026

The user's target for the next month is a usable environment for real
workloads, a browser, initial physical-hardware testing, Vulkan and Proton.
These are ordered development targets, not claims that support already works.

| Priority | Milestone | Evidence needed before moving on |
| --- | --- | --- |
| 1 | Environment polish and VM tooling | VM MCP works for user/agent launches; screenshots/input/logs/profile/stats are usable; panic evidence survives and can be inspected; fresh builds and experiment disks remain reproducible. Resolve the persistence and UI issues needed by real workloads. |
| 2 | Browser usability | Bring up Ladybird first and verify navigation, text entry, scrolling, rendering and stability. Then evaluate Firefox or a fork against Brook's actual ABI/runtime gaps. |
| 3 | First physical-hardware test | Boot with software rendering and validate display/input/workload basics. Network and audio remain disabled for this first test. Record a hardware inventory, recovery path and reproducible boot media. |
| 4 | Vulkan rendering | Follow the [Venus bring-up plan](vulkan-bringup.md): host/Linux control, blob memory and synchronization, deterministic offscreen rendering, Wayland presentation, then a bounded workload with hardware-renderer evidence. |
| 5 | Proton and Windows apps | Audit Wine/Proton runtime requirements against Brook; reach a minimal Windows app first, then expand to a representative workload with graphics/input coverage. |

Use [backlog.md](backlog.md) to track concrete blockers. Browser, real-hardware,
Vulkan and Proton investigations remain separate work after the bridge; this
MCP implementation does not certify them. Preserve raw captures and diagnostic
artifacts outside git, and keep the licensed Quake assets out of commits.
