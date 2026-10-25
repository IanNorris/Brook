# Quake III Arena demo on Brook

The native64 ioquake3 engine runs a local `q3dm1` match with a Sarge bot on
Brook. This is a bounded smoke test using original demo assets plus public
point-release data, not acceptance of the retail game or multiplayer.
No kernel changes were needed. Data and configuration remain private under
`build/workloads/quake3-20261025` and the guest's dedicated test home.

## Engine and data

The repository's pinned nixpkgs supplies `ioquake3-0-unstable-2025-05-15`, store
output `0hqh1rdawmbyhv488hv7jlkbyrg98dj0`. It was exported using
`scripts/export_guest_package.py` and installed through guest `nix-fetch --deps`
from the private local cache. The native engine and renderer remain unchanged.

The original id Software installers are available from the
[GWDG archive mirror](https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake3/linux/):

- `linuxq3ademo-1.11-6.x86.gz.sh`, SHA-256
  `64dee3f69b6e792d1da4fe0ac98fedc7eb1e37ea1027fb609a9fadd06150a4ec`.
- `linuxq3apoint-1.32b-3.x86.run`, SHA-256
  `c36132c5556b35e01950f1e9c646235033a5130f87ad776ba2bc7becf4f4f186`.

Only data is extracted; neither old installer nor its i386 engine is executed.
The demo's `demoq3/pak0.pk3` is placed in the private `baseq3`, alongside the
public patch's pak1 through pak8. Matching native64 `ui`, `cgame` and `qagame`
modules from the ioquake3 package replace use of the incompatible demo QVMs,
with `vm_ui`, `vm_cgame` and `vm_game` set to zero. This follows the demo-data
approach described in the [ioquake3 player guide](https://ioquake3.org/help/players-guide/).
The demo pak itself is unchanged; its SHA-256 is
`e77abad2466f45a0a7ea018445528f9b95a0fe7789fa1abc1a7718bbf0754b08`.
The initial attempt with only pak0 correctly stopped requesting point-release
data; it is excluded from gameplay acceptance. No game data is committed.

## Verified behavior and limits

The test uses the existing capsule-v22 VM with eight vCPUs and 12 GiB.
SDL uses Wayland and the `opengl2` renderer reports Mesa VirGL on Intel UHD 630,
OpenGL 4.6 Core Profile. The window's client area is 1280x720, VSync is off and
the engine cap is 125 FPS. Captured counter readings are approximately 39–44
FPS; these are observations during interactive capture, not a timed benchmark.

The arena, bot, animated scene and HUD render. Keyboard movement changes the
view, firing consumes ammunition, and the bot navigates and frags the player.
Escape opens the game menu. Keyboard navigation selects Exit Game; confirming
Yes displays the credits, and Enter completes a normal zero-status exit.
All tracked descendants exit and both bridge output streams reach EOF.

The test disables mouse grabbing with `in_nograb=1`. An absolute bridge click
in the game menu selected a different item from the one under the visible host
cursor; mouse/menu alignment and captured relative input remain unaccepted.
Keyboard menu navigation works. Fullscreen, persistence, other maps and network
play were not tested.

Audio uses SDL's ALSA backend and the existing clocked OSS plugin, with OpenAL
disabled. A four-second gameplay capture contains 179,904 stereo frames at
44.1 kHz, duration 4.079 seconds and RMS 0.0366. A two-second post-exit capture
reports an inactive stream and zero frames. This proves non-silent device
output and cleanup, not subjective pacing or freedom from stutters. The demo
does not contain the requested `music/sonic5.wav` map track; missing grenade
sound warnings also remain. Music playback is not claimed.

![Quake III Arena demo with a bot on Brook](images/brook_ioquake3_demo_1080.png)
