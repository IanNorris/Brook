# Brook documentation

Start with the [project README](../README.md), then:

- [How Brook is put together](architecture.md)
- [Building and running](building.md)
- [Hardware support](hardware.md)
- [Past problems and their fixes](past-problems.md)

The pages below are engineering notes written as the work happened. They
explain how things were designed and verified, but may lag the code.

## Desktop and graphics

- [Clearwater design](design/clearwater-next/DESIGN.md): the desktop's look and behaviour
- [Compositor direction](compositor-direction.md): why the kernel owns the display
- [App-driven presentation](app-driven-present.md): frame pacing and direct scanout
- [Vulkan desktop](vulkan-desktop.md)
- [Xwayland window integration](xwayland-window-integration.md)
- [Wayland configure handling](wayland-configure.md)
- [Pointer capture](pointer-capture.md): pointer lock for games
- [Broadwell page flipping](bdw-page-flip.md) and [zero-copy scanout](bdw-zero-copy-scanout.md)
- [Broadwell video engine](bdw-video-engine.md)
- [GPU hang diagnostics](gpu-hang-diagnostics.md)

## Apps and games

- [App Gallery](app-gallery.md)
- [Chromium sandbox](chromium-sandbox.md): why browsers run unsandboxed
- [Stardew Valley acceptance log](stardew-native64.md) and [Torchlight II, Factorio and Warzone 2100](three-games-checkpoint-20261026.md)
- [Quake III](quake3.md)
- [Wine](wine-smoke.md), [Unity games under Wine](wine-unity.md), [DXVK](wine-dxvk.md), [Proton](proton.md) and [Direct3D 12](d3d12.md)

## System

- [Users and permissions](identity.md) (a detailed audit)
- [Orderly shutdown](orderly-shutdown.md)
- [Out-of-memory recovery](oom-recovery.md)
- [Process core dumps](process-core-dump.md)
- [vDSO clock](vdso-clock.md)
- [Compressed image format](compressed-image-format.md)
- [Register and stack tests](register-state-tests.md)
- [Building Brook on Brook](bootstrap.md) (a long, detailed log)
- [Promoting a new boot loader on the Dell](dell-loader-promotion.md)
- [Recording a VM](vm-recording.md)
- [VMware](vmware.md)
