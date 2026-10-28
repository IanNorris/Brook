# Software and redistribution inventory

Compatibility does not grant redistribution rights. These results describe our existing workload tests; a new portable image must pass its own acceptance. The initial image uses software graphics, so hardware VirGL/Venus results do not imply equivalent performance here.

## Selected base

Chromium, GIMP, VLC, OpenTTD (OpenGFX/OpenSFX/OpenMSX), Mousepad, Qalculate!, Vim, TCC, Python, Git, Bash/coreutils, FFmpeg and Brook native applications. The exact transitive closure is recorded in the generated manifest. No previous user image is an input.

## Tested software

- **bash** — ✅ Working. Bundled shell and Nix Bash closures; readline, job control and scripting Route: base-image.
- **DOOM** — ✅ Working. Runs in a WM window or full-screen, with keyboard input Route: engine-only; proprietary game data excluded.
- **Quake 2 / Yamagi** — ✅ Gameplay. Software and hardware virgl GL3 renderers, startup demo, OSS audio and OGG music; [GPU resource retirement/repeated launches](docs/gpu-resource-lifetime.md), mouse capture and intermittent audio timing remain open Route: engine-only; proprietary game data excluded.
- **GLtron** — ⚠️ Experimental. Compatibility launcher installs through guest `nix-install`; hardware virgl menu and light-cycle gameplay verified, with sound disabled Route: optional-open-source; component/asset audit required.
- **SuperTuxKart** — ⚠️ Experimental. Guest installation and hardware virgl races verified; current ownership build accepts race input and [reclaims backing after close](docs/gpu-resource-lifetime.md#gpu-workload-transition-acceptance), with clean subsequent Gallery/browser surfaces; launcher sound is disabled, memory pressure and mixed-game acceptance remain open Route: optional-open-source; component/asset audit required.
- **Vulkan / Mesa Venus** — ✅ vkcube + vkmark. Intel hardware compute, rendering and direct Wayland DMA-BUF presentation; automatic staging with CPU composition. Repeated window lifecycle and concurrent GL verified. [Results and limits](docs/vulkan-desktop.md) Route: optional-open-source; component/asset audit required.
- **busybox** — ✅ Working. ls, cat, echo, wc, head, tail, grep, and many more Route: legacy-optional; replaced by coreutils in this image.
- **TCC** — ✅ Working. Compiles and runs C programs natively on Brook Route: base-image.
- **CoreMark** — ✅ Working. CPU benchmark, runs to completion Route: optional-open-source; component/asset audit required.
- **musl libc** — ✅ Working. Standard C library, dynamically linked Route: base-image.
- **Go runtime** — ✅ Working. Static Go binaries (lazy PROT_NONE heap + on-demand mprotect) Route: optional-open-source; component/asset audit required.
- **Nix binaries** — ✅ Working. Stock NixOS closures fetched via `nix-install` (cowsay, coreutils, curl, …) Route: base-image.
- **NetSurf** — ✅ Working. Web browser rendering HTML/CSS via custom framebuffer surface Route: optional-open-source; component/asset audit required.
- **Ladybird** — ⚠️ Experimental. HTTPS Google search and Amazon navigation verified; loading/input/scrolling performance remains limited Route: optional-open-source; component/asset audit required.
- **Firefox** — ⚠️ Experimental. Included in fresh experiment images; software Wayland HTTPS/JavaScript and YouTube audio through the normal launcher, including pause/resume; synthetic site storage survives reboot; hardware child-surface rendering and local video/PCM capture now verified; low video framerate and intermittent audio gaps remain Route: user-install-or-unbranded-build.
- **Chromium** — ⚠️ Experimental. Software-rendered Wayland browsing and 480p YouTube video/audio; long pause/resume and seeking verified under normal autoplay rules; browser sandbox disabled Route: base-image.
- **OpenTTD** — ⚠️ Experimental. World creation and cold-boot save/load verified; music output, normal quit and forced-exit cleanup checked; longer-session gameplay remains experimental Route: base-image.
- **VSCodium** — ⚠️ Experimental. Brook-user editing, cold save/reopen and normal exit verified alongside Firefox and Chromium in a 4 GiB VM; startup remains slow; Electron sandbox disabled Route: optional-open-source; component/asset audit required.
- **Blender** — 🚧 Blocked. Guest installation and LLVM dependency correction verified; audio initialization and EGL rendering remain blocked Route: not-in-image; compatibility-blocked.
- **Wine64** — ✅ Basic apps. Console command and Windows Notepad: keyboard input, save, normal close and cold-boot reopen as Brook. Broader Windows coverage remains limited. [Evidence and screenshot](docs/wine-smoke.md#console-and-notepad-acceptance--19-october) Route: optional-open-source; component/asset audit required.
- **Wine + DXVK** — ⚠️ Experimental D3D11. Hardware Venus shader/readback probe and Windows SDL3 Snake; keyboard input, resize, maximize, minimize/restore and repeated launch/close verified. Per-app Unity audio also verified; standalone Proton GE has a separate smoke test. [Results](docs/wine-dxvk.md), [Unity smoke test](docs/wine-unity.md) Route: optional-open-source; component/asset audit required.
- **Proton GE** — ⚠️ Experimental. Standalone Helltaker at 720p: first-puzzle input/reset, captured audio and normal exit verified. Later Steam integration results are recorded separately in docs/steam-library-stability.md. [Results](docs/proton.md) Route: optional-open-source; component/asset audit required.
- **Steam** — ⚠️ Experimental. QR sign-in, Library, downloads and verification tested through SteamRT. Native64 Stardew gameplay and save/reload across reboot work; game audio and bounded timing-fault recovery verified. Startup remains slow and variable. [Game results](docs/stardew-native64.md), [client limits](docs/steam-library-stability.md) Route: publisher-download-only.
- **Stardew Valley** — ✅ Compatible. Gameplay verified by live user testing through Steam; older saves, sleep-to-save and day-2 reload after a clean VM reboot tested. Loading times remain slow and variable. [Results](docs/stardew-native64.md) Route: user-owned-import.
- **Torchlight II** — ⚠️ Experimental. Character creation, 3D gameplay, movement, save/resume and captured audio in a controlled engine launch; requires a UTF-8 locale archive and an active Steam session. The mod launcher can stall waiting for Steam. [Results](docs/three-games-checkpoint-20261026.md) Route: user-owned-import.
- **Factorio demo** — ⚠️ Experimental. Tutorial gameplay, movement, fresh save/reload, captured audio and normal exit verified through hardware VirGL; low-memory graphics settings required, startup about 73 seconds. [Results](docs/three-games-checkpoint-20261026.md) Route: publisher-download-only.
- **Warzone 2100** — ⚠️ Experimental. Hardware VirGL tutorial/campaign rendering, unit input, fresh campaign save/reload, captured audio and normal exit verified. [Results](docs/three-games-checkpoint-20261026.md) Route: optional-open-source; component/asset audit required.
- **D3D12 / VKD3D-Proton** — ⚠️ Experimental. Hardware shader/readback, event fences, 720p swapchain resize, Windows SDL3 Snake and Veloren 3D singleplayer through Proton GE. Per-game native shader compiler required for Veloren. [Results and screenshots](docs/d3d12.md) Route: optional-open-source; component/asset audit required.
- **SpaceVenture (Windows)** — ⚠️ Experimental. D3D11/DXVK main menu, opening sequence and first-room interaction; clocked ALSA PCM captured. [Screenshot and limits](docs/wine-unity.md#spaceventure--unity-2022362f2) Route: user-owned-import.
- **Helltaker (Windows)** — ⚠️ Experimental. 720p D3D11/DXVK first-puzzle gameplay, keyboard movement/restart, minimize/restore and normal exit; clocked ALSA audio captured. [Screenshot and scope](docs/wine-unity.md) Route: publisher-download-only.
- **Qalculate!** — ✅ Working. GTK3 calculator via Wayland (CSD decorations) Route: base-image.
- **GIMP** — ⚠️ Experimental. Painting, XCF save/reopen (including cold boot) and PNG export/import verified; Brook-owned legacy profile, settings and saved images verified across reboot; optional plugin discovery still has faults Route: base-image.
- **LibreOffice** — ⚠️ Experimental. Writer save/reopen across cold boot and full native-close cleanup verified as Brook; startup and broader suite coverage remain experimental Route: optional-open-source; component/asset audit required.
- **VLC** — ⚠️ Experimental. 720p OSS playback; Wayland black flashes fixed; cold 55-second minimize/restore, normal close and SIGKILL audio cleanup verified; audio timing still experimental Route: base-image.
- **Mousepad** — editing tested.  Route: base-image.
- **Vim** — interactive ASCII edit/save/reopen and shell return tested.  Route: base-image.
- **Alpaca Stacka** — launch attempted; loading/performance remains unresolved.  Route: Steam user download only.
- **Quake III Arena** — gameplay/mouse capture and audio tested; low FPS remains.  Route: open engine optional; user-owned game data only.
- **Freedoom** — not accepted with this image yet; do not inherit original DOOM data acceptance.  Route: optional-open-source; validate before inclusion.
- **Half-Life / Left 4 Dead** — 32-bit requirements prevent current native64 acceptance.  Route: not-in-image.
- **Graveyard Keeper** — proposed, not accepted.  Route: not-in-image.

## Publication requirements

The generated bundle is a private candidate until its exact source and notice pack is complete. `scripts/audit_distribution_sources.py` records runtime derivations, copied font/firmware inputs, available sources and missing inputs. It is evidence for review, not an automatic licence clearance. Include matching Brook source/submodules, pinned Nixpkgs recipes, patches, vendored/generated code, component notices and exact required corresponding source beside any public binary download. Do not replace this with a promise to provide source later.

Firefox is excluded from this base: our modified branded build needs the Mozilla distribution/trademark conditions addressed or a properly unbranded build. Steam, Factorio demo and free Steam games are publisher downloads; their payloads are not in this image. Paid games, original Quake/DOOM data, music, account data and saves are excluded.

Primary policy references: [Mozilla](https://www.mozilla.org/en-US/foundation/trademarks/distribution-policy/), [Factorio](https://www.factorio.com/terms-of-service), [Steam](https://store.steampowered.com/subscriber_agreement/), [GNU corresponding source](https://www.gnu.org/licenses/gpl-faq.en.html#DistributeExtendedBinary), [OpenTTD installation](https://wiki.openttd.org/en/Manual/Installation).

## Build and run

Inside `nix-shell`:

```sh
python3 scripts/create_experiment_disks.py --public --profile-expression nix/release-packages.nix --nix-gib 12 --data-gib 4 --output build/distribution/new-seed
python3 scripts/make_public_qemu.py --seed build/distribution/new-seed --output build/distribution/new-bundle
```

Both output directories must be new. The assembler allowlists the ESP files and builds one GPT/QCOW2 disk from the new seed. Existing private review disks are never copied. Read `tools/portable_qemu/README.txt` for prerequisites, launch options, persistent instances and MCP configuration. Host QEMU binaries are deliberately not included yet; Windows/macOS launchers need native-host acceptance.

## Candidate acceptance — 27 October 2026

The first profile contained 4,163,928,264 NAR bytes. Keeping shared LLVM libraries while excluding its static development archives reduced this to approximately 3.79 GB, without changing the tested driver code. The compressed QCOW2 is approximately 1.50 GB; virtual capacity is about 16.6 GiB (4 GiB data, 12 GiB Nix, 512 MiB temporary volume and EFI).

Linux KVM (eight CPUs) and TCG (two CPUs) boot and normal-user shell/file/TCC/Python/Git checks passed. The smaller LLVM/Gallium libraries load in Brook. VLC played a generated three-second test clip to completion. The launcher and Mousepad input were visually checked; saving through Mousepad's shortcut was not confirmed in that pass. Windows and macOS launchers remain unverified on native hosts.

Fresh-image fixes include writable private UEFI variables, an explicit RAM-backed boot script/Bash/wallpaper, ext2-backed temporary sockets, profile-relative graphics paths, and tablet scaling after resolution changes. Per-widget taskbar preferences are in Settings: CPU and memory default on; disk, network and compositor/app frame histories default off. Settings persist in the normal user configuration.

A close/save confirmation can hold orderly shutdown open. The portable bridge keeps screenshots/input available while a power transaction waits; it never equates closing QEMU with clean guest shutdown. Forced stops remain explicitly labelled. Reboot/control reconnection is still under final acceptance; consult the generated acceptance report rather than assuming all MCP capabilities match the Linux development bridge.

The separate source-input archive includes the pinned Nixpkgs recipe tree and exact third-party source/patch/vendor inputs. Matching Brook source/submodules are supplied separately. Publication still requires the component notice review; the image remains a private candidate until that is completed. QEMU is a host prerequisite, not bundled binary code.
