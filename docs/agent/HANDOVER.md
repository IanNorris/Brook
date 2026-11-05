# Brook project handover

Continuation update, 5 November 2026: read-only onboarding and documentation installation are complete. Ian authorized physical recovery testing; keep the old chat paused and available. After a graceful restart the keyboard and touchpad failed in Brook, but both worked again after a checked shutdown and full power pull. The cause is unconfirmed. On the subsequent boot Brook reports the SSD recovery files and both loaders present, with loader/configuration hashes matching the transfer snapshot. Ian reported that the EFI shell listed only BOOTX64.EFI in EFI/BOOT on both visible filesystems; the identity of those shell mappings versus Brook’s /media/system and the discrepancy remain unresolved. F5 recovery boot remains unverified. The branch and other hardware observations below describe the transfer snapshot unless explicitly updated.

The original Brook goal was paused at 01:19:47 UTC on 5 November 2026, and its last turn completed at 01:19:56. Ian stopped it at a safe point for transfer. [evidence/snapshot.json](/data/Programming/DSHScratch/brook-handoff/evidence/snapshot.json) records the final repository state; [evidence/final-state.json](/data/Programming/DSHScratch/brook-handoff/evidence/final-state.json) preserves final messages and completed operation results. Keep the old chat paused and available through the transition; do not archive or delete it. Wait for Ian before the physical recovery boot test.

## Current goal update, 5 November 2026

Ian authorized unattended work on the Dell tonight: prioritize reliable recovery after failures and collecting failure evidence, then implement physical Intel HD accelerated 3D and prove it using existing yquake2 GL and Vulkan tests. If the Dell is offline and blocks further useful work, pause for Ian to reboot it. The former chat remains paused.

Ian expanded the recovery goal: make the recovery tooling available in the normal boot, automate recovery wherever feasible, enlarge boot text without undue complexity, and add timed hang/panic recovery. Capture panic details through the bridge where feasible and in a preallocated disk area that does not require filesystem metadata changes at failure time. Keep the separate recovery and USB paths as fallbacks. This request supersedes the earlier opt-in-only watchdog direction as a product goal; its safe arming, inhibition and loop prevention still need a design appropriate to automatic operation. A fully wedged kernel cannot be guaranteed to run a software handler.

Physical results: Ian created a persistent firmware entry for BROOKF5.EFI and selected F5. Recovery boot `cb5b56e1a4e04858bd7c280c8af2d045` advertised build b4210c41 with the writable desktop disabled; its automatic read-only SSD check completed cleanly. Both diagnostic shortcuts appeared in the launcher catalog. Remote NETCHECK completed with exit 0 and a framed local bridge reply. Ian reported keyboard and touchpad unresponsive on this recovery boot, so local launcher interaction remains unverified. Earlier both inputs worked again after checked shutdown and a full power pull. The pre-F5 normal boot had reached the desktop; independent original-default and USB fallback checks remain to be assessed.

Current implementation progress: KernelPanic now emits the structured serial panic packet without a framebuffer, and the serial staging buffer accommodates its header. Host regression decodes the headless packet and kernel objects compile. This is not deployed or boot-tested, now also preserves structured serial output in the tested framebuffer exception path; it does not implement disk capture or panic network transport. The QEMU-only reset experiment is described below.

AHCI capture prerequisite: normal command/DMA operations now enter an atomic gate under their existing mutex. A panic claimant may acquire the idle gate without waiting and retain it until reboot; it must skip capture when ordinary I/O owns it. Host race tests pass; a disposable QEMU boot mounted an identity-bound SATA fixture and wrote, synced and independently read back a file. No panic claimant or disk writer is connected yet, and no physical hardware was changed.

Crash capture preparation: the structured packet is available in boot-lifetime static memory. A two-slot fixed-sector journal stores up to 64 KiB per capture with CRC, data-first flush/readback and header-last publication; host fault injection covers read, partial write and flush failures while preserving the prior slot. A host test round-trips an actual headless panic packet through it. This journal is not connected to a device, the filesystem, or physical storage; its memory-disk tests do not prove SSD durability. AHCI uses scheduler-aware locking and shared DMA, so a panic-safe ownership and I/O path remains necessary before deployment.

Panic-reset experiment: a QEMU-fw_cfg-only test path waits one second after a panic dump and issues the selected reset once; it is disabled on the Dell. Disposable TCG/OVMF runs passed a deliberate panic, an unarmed control, a #UD exception and an actual second boot. Both panic and exception emitted PANIC_HEX. The fixture kernel and its optional audio module have different ABI versions; the module was rejected and was not involved in these tests. The test does not establish physical timing, data integrity, hang recovery, durable capture, reset inhibition or reboot-loop prevention.

Current todo:
1. Investigate input failures across warm restart and recovery; restore reliable local interaction.
2. Integrate normal-boot diagnostics with ownership-aware filesystem checks and verify remotely and locally.
3. Implement automatic, identity-bound repair/recovery with bounded retries and preserved fallback paths; existing guarded repair machinery is experimental, not production-ready.
4. Design and implement timed hang and panic recovery, panic transport and preallocated durable crash records, with bounded failure handling and update/repair inhibition.
5. Improve boot text readability and validate recovery behavior on disposable media and then the Dell.
6. Implement physical Intel HD 3D acceleration and validate with existing yquake2 GL and Vulkan tests; distinguish real hardware execution from VM and simulation evidence. Record limits and stop once the full goal is verified.

## Project and working relationship

Brook is Ian Norris's hobby x86-64 operating system, built from scratch in C++ with Clang/LLVM. It boots with UEFI, supports SMP and loadable drivers, has a native desktop with a Wayland compositor, and implements a growing Linux syscall ABI so unmodified Linux applications and Nix closures can run. This is a custom OS, not a Linux distribution.

The development host is Linux with a pinned Nix environment. The repository inspected here is `/data/Programming/Codex/Brook`. Its README documents `nix-shell`, `./scripts/build.sh Release` and the broader `build_all.sh`; select the smallest relevant target after consulting the task's existing notes. Do not run the full image and application pipeline merely to establish that the handover works.

Ian values clean architecture, usable applications, reproducible fresh builds and practical progress. He actively tests milestones and supplies hardware observations. He has repeatedly requested bounded investigations and discouraged diminishing returns. Preserve the reasons behind prior decisions, distinguish provisional experiments from accepted defaults, and avoid rebuilding a private collection of undocumented helpers.

## Debug bridge and MCP

The existing `brook-vm` stdio MCP connection serves separate `vm_*` QEMU and `hw_*` physical-machine tools. Read [DEBUG_BRIDGE.md](DEBUG_BRIDGE.md) for the host registration, registries, authentication distinction, last-session references and operational limitations. Use the installed `$brook-vm` skill, or its bundled [entrypoint](../../tools/vm_bridge/skills/brook-vm/SKILL.md), before operating a guest. Read only the reference for the selected workflow. This is part of read-only onboarding; discovering how the bridge works does not require a new VM, guest probe, test, transfer or reboot.

## Current priority and wider programme

The latest operational requests are:

1. Put the recovery environment on the Dell SSD, including tools in the boot partition and diagnostics for a failed bridge.
2. Provide launcher shortcuts for a diagnostics and recovery suite Ian can use locally; the same checks should be useful remotely while the bridge works.
3. Consider an opt-in three-minute bridge-loss watchdog. Its design is recorded, but it is not implemented.

The validated Dell/Steam milestone was already promoted to main on 4 November, according to the 11:47 session report. The observed local `main` points to `519f3ea3` and the locally stored `origin/main` tracking ref to `ba25a11e`; this preparation did not fetch or verify the live remote. The wider programme now concerns subsequent Steam reliability/usability, individual X11 windows in the Wayland desktop, and Intel HD graphics support. The old automatic goal text still names the completed promotion; do not restart that promotion or its acceptance cycle. The SSD recovery request is the immediate priority.

## Repository state and active work

The final observed branch is `work/steam-desktop-hardening` at `7d21d1aaab5760995c5d4deb7530a937d2fd8ada`. The worktree is clean and HEAD matches the locally stored `origin/work/steam-desktop-hardening` tracking ref; no remote fetch was performed. The original goal is explicitly paused and the indexed latest turns for its 21 known descendants are all terminal. This describes the transfer snapshot before repository housekeeping began.

Recent work already includes:

| Change | Commit | Scope |
| --- | --- | --- |
| Minimal hardware connectivity probe | `b4210c41` | Local bridge/network diagnosis |
| Recovery image preparation and launcher tools | `a2532e51` | Candidate recovery pair and diagnostic shortcuts |
| VFS mount publication identity | `04c8e5a8` | Identity support for the publisher |
| Admitted SSD system mount identity | `68a8e7da` | Bind staging to the admitted filesystem |
| Retained vnode versus published mount root | `2efb7b71` | Root identity verification |
| Bind SSD verification to admitted FAT device | `8260bd80` | Device identity check |
| SSD boot-root verification ioctl | `bd87c6d0` | Publisher can check an open directory |
| Guest ESP root probe | `1a1dd487` | Positive and overmount/alias controls |
| SSD recovery staging | `7d21d1aa` | Publisher implementation and host/QEMU checks |

The preparation snapshot's uncommitted ioctl/probe work is now committed, and the publisher exists in `tools/hardware_ssd_recovery_stage.c`. Its physical staging job completed with exit zero, no remaining job descendants and `SSD_RECOVERY_STAGE_OK bootloader unchanged`. Do not reimplement the interface or publisher, repeat staging, or replay completed transfer/job IDs to begin the continuation.

## Dell and recovery state

The target is a Dell XPS 13 9343 with Broadwell graphics and a Samsung PM851 256 GB SSD. Obtain current ATA serials, GPT identities, boot IDs and connection details through the existing private manifests and boot-bound tools when needed; model names and old addresses are insufficient for physical writes.

**The latest installation note records a 256 MiB EFI partition plus a separate data partition using the remaining SSD.** It replaced an older layout on 3 November. Older notes describe a 6 GiB system volume, a store image, A/B generation activation and prior installation attempts. Those passages are history and cannot establish the current layout. Read the latest dated installation checkpoint first.

The 4 November recovery sequence repaired the USB store and SSD data filesystem, ran fresh read-only checks, and restarted the Dell to its SSD. The final 5 November observation at 01:18:56 found the bridge reachable, boot identity matched, and the SSD desktop volume ready. The last recorded boot ID is in final-state.json. Discover current runtime state before later hardware action; do not assume this boot or connection is still live.

The SSD's default EFI loader predates the F5 recovery menu and remains unchanged. The recovery kernel/initrd, static filesystem checker, read-only SSD-check supervisor and network/bridge diagnostic with launcher entries are now staged and verified on the physical SSD. Their QEMU boot and launcher evidence remains reusable; the physical F12/F5 recovery boot has not happened.

Reuse fingerprints from `build/hardware/ssd-local-recovery-tools-v2-20261105/manifest.json`: it records the kernel/initrd hashes and diagnostic/checker identities. The menu fixture is `build/hardware/ssd-recovery-menu-final-20261105/fixture.raw`; the original transcript records its `test_recovery_menu_boot.py` invocation at 00:22:15 UTC on 5 November and the resulting summary at 00:23:38. The launcher evidence is under `build/hardware/qemu-recovery-diagnostics-20261105/`. Consult these existing artifacts to match the candidate being reused. The preparation manifest itself says not installed or boot tested; pair it with the later boot evidence rather than treating it alone as proof. An unrelated publisher change does not invalidate unchanged payload tests.

The Dell successfully booted the matched `dell-ssd-recovery-stage-20261105` kernel/driver payload before staging; its manifest under `build/hardware/dell-ssd-recovery-stage-20261105/manifest.json` identifies ABI 65 and the prepared kernel/initrd. Use the subsequent boot results in final-state.json with that preparation manifest.

The physical SSD now has these paths under the guest's `/media/system` mount:

| Path | Final recorded state |
| --- | --- |
| `RECOVERY/KERNEL.ELF`, `RECOVERY/INITRD.IMG`, `RECOVERY/SELECT.BIN` | Staged and read back successfully |
| `EFI/BOOT/BOOTX64.EFI` | Original default loader retained; SHA-256 `cc1989c81db48d5ffc40d0f72e63faf55048090dbd297575a314a5f81d3fba09` |
| `EFI/BOOT/BROOKF5.EFI` | Separate candidate loader; SHA-256 `8f29b69f638c144fd29578c08f9062095c02457bf3d8b2578f6b8258887161b1` |
| `BROOK.CFG` | Original prefix retained and `RECOVERY_MENU=1` appended; final SHA-256 `28663348de053b50f5102c3c945a74bbba599a034f1d8c2cc724f58ae5af6115` |
| `CFGPRE.BAK` | Original configuration backup; SHA-256 `7564bd7ca02f64d2f138d6166256e59367bfb8ce0fb1b5fdac00619e2f481096` |

The final loader/configuration job completed with exit zero, matching hashes and no live descendants. An earlier attempt stopped because `grep` and `cmp` were unavailable on the guest PATH; its partial backup was reconciled before the successful hash-based operation. Use known available utilities rather than replaying that failed script. Keep the USB recovery path and all listed rollback assets intact.

## Acceptance for the recovery milestone

The recovery pair and required diagnostic tools are published to the admitted SSD boot partition with identity verification, space checks, ordered write/flush/readback and a retained loader backup. The F5-capable loader can select that verified recovery pair, and the normal desktop path still boots. Ian can launch local diagnostics without a functioning host connection. While the bridge is up, the host can also invoke the guest diagnostic and retrieve its result. Reports distinguish host reachability, guest interface/network status, bridge failure and unknown state. A guest loopback reply covers only the guest-side path. USB recovery remains available.

The existing host-side `scripts/probe_hardware_link.py` checks route, ping, neighbor state and a framed hardware-capabilities response at a specified endpoint. Its failures do not distinguish a guest hang from link loss or a panic. Combine that evidence with the guest-local diagnostic; do not treat a successful TCP connection as a healthy bridge.

Use the existing disposable-image evidence for already tested loader and image behavior. Exercise the integrated publisher and any newly introduced failure paths. Physical acceptance should establish the actual SSD recovery selection, tool launch and retained normal boot. Preserve the expected guarantees and state their limits; arbitrary power-loss tolerance is not established by stopping QEMU.

The watchdog is a separate follow-up unless Ian changes the milestone. Its current design requires explicit arming for the current boot, heartbeats through the actual bridge, three-minute expiry, observable status, inhibition during repair/update/flush/shutdown, and no automatic rearm loop after restart. It cannot guarantee recovery from a completely wedged kernel.

## Established capabilities and important limits

- The native QEMU development path supports a substantial Linux application stack, browsers, Wine/Proton and VirGL/Venus experiments. Virtual GPU successes do not establish a physical Intel graphics driver.
- Steam authentication, library operations and native64 Stardew gameplay/save/reload have prior successful evidence in QEMU, including Ian's gameplay acceptance. Preserve those results. Dell Steam reliability is a separate unresolved question.
- Rootless Xwayland and Brook's private XWM already exist. Individual Steam windows have rendered in native desktop windows, with focus, geometry and close controls. Broad Dell acceptance remains incomplete. Do not restart the architecture from nested Xephyr; keep nested X as a diagnostic fallback.
- The missing Xwayland `setitimer` interface is real. `-dumbSched` suppresses its repeated unsupported calls, but the comparisons did not establish a Steam performance or Dell stability improvement. Keep that conclusion narrow.
- The Broadwell work includes guarded forcewake observations, command/context preparation, mapping/lifetime gates and simulated submission machinery. It does not yet establish a working physical GPU execution backend.
- Network/bridge loss and recurrent dirty filesystems have occurred. A missing bridge is not proof of a guest crash; use local diagnostics and the documented recovery path. Do not replay uncertain writes or restart jobs solely after a timeout.

## Onboarding and remaining todo list

The original onboarding and recovery acceptance list below is retained as historical context. The current expanded todo is at the top of this document.

1. With Ian available, determine whether the Dell's F12 firmware menu can select the separate `EFI/BOOT/BROOKF5.EFI` loader. Its availability through F12 is not yet proven; do not silently replace the default loader to bypass that question.
2. Boot that loader and verify the F5 recovery path on the physical Dell using the already staged pair.
3. Verify local launcher diagnostics, retrieve a diagnostic remotely while the bridge works, and establish the retained normal desktop boot path.
4. Record the result and remaining material limitations, then stop when the bounded recovery goal and its todo list are complete. Ask Ian to select the next goal; the watchdog and wider Steam/graphics backlog remain separate.

The old session must remain paused and available through onboarding and the transition. A question asking whether session management is finished remains in the old chat; do not answer it with a resume instruction as part of onboarding the new chat. Live hardware state may be rediscovered when the physical test is authorized. No update or reboot was in progress at the old session's reported stopping point; this handover does not claim that every idle VM or ordinary OS process has exited.
