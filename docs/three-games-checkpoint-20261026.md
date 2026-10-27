# Torchlight II, Factorio demo and Warzone 2100

This bounded follow-up uses a fresh private copy of the checked PI/Goose review
disks, kernel SHA256
`4e47f74440f0a2f5da43a1ad228da5c54368cff3a61dd5a379fb72c169fa4826`,
eight vCPUs, 8 GiB RAM, Intel hardware Venus/VirGL, native SDL display and the
existing 4,096-frame audio allowance. Original licensed data, account state and
saves stay in ignored review images. Each additional game uses fresh test data.

## Preparation

- Torchlight II uses its normal guest Steam installation, app 200710. Cached
  launch metadata lists a Linux x86_64 mod launcher; actual downloaded launcher,
  engine and library headers remain the architecture gate.
- Factorio's publisher-provided Linux x64 demo is version 2.1.23, downloaded
  from `https://factorio.com/get-download/2.1.23/demo/linux64`.
  Archive SHA256:
  `66e1e4bcc892b97b145fb57f20952329a884764848fc261aad6ffa89604e5f22`.
  The extracted player is ELF64/x86-64, interpreter
  `/lib64/ld-linux-x86-64.so.2`, SHA256
  `9c582f3cb91c7505668e7190fe8e40dc4ef678bfd1f4406e226f5e5e1d021429`.
  Transfer into the guest independently checks the complete archive hash.
  [Publisher download](https://factorio.com/download).
- Warzone 2100 uses the pinned Nixpkgs 4.7.0 package. Disabling its optional
  movies changes the derivation and rebuilds expensive texture assets; that
  preparation build was cancelled in favor of the unchanged package available
  from the official Nix binary cache. This is a preparation choice, not a
  compatibility fix. [Upstream release](https://github.com/Warzone2100/warzone2100/releases/tag/4.7.0).

Installation, game loading and gameplay are separate observations. A Steam CLI
acknowledgement, window presence or nonzero audio samples alone does not establish
full compatibility. Rendering, input, save/reload and normal exit are checked
individually below as evidence becomes available.

## Findings so far

The native Torchlight launcher and its real engine are ELF64/x86-64. Steam
installs build 24292952, Linux depot 200715 manifest 3414435834532674516.
The launcher renders and its Play action reaches the game title/background.
The default 1920×1080 window extends beyond the desktop. A later controlled
engine launch uses the default 800×600 window, keeping its controls reachable.

Factorio's first startup fails in `std::filesystem::status` with EBADF for an
empty pathname. A scoped file trace ends with `newfstatat(AT_FDCWD, ..., ...) =
-9`. An owned static probe reproduces incorrect empty-path and flag behavior.
The kernel correction returns ENOENT without AT_EMPTY_PATH, handles cwd and
descriptor empty-path metadata separately, and follows Linux 6.12's NULL-path
fast-path distinctions. Review also identified that cwd metadata must use a
retained path without requiring directory search permission. The expanded
28-check control passes both on Linux 6.12.112 and the final native kernel,
including cwd metadata identity and the no-search-permission case.

The first corrected Factorio launch initializes hardware VirGL OpenGL 4.6 and
private PulseAudio at 44.1 kHz, then reports GL_OUT_OF_MEMORY during atlas
creation. Kernel logs show resource backing attachment failures for large
resources; the rolling allocator failure counter is zero. This is not evidence
of exhausted host VRAM or failed physical allocation. A bounded retry with
`--force-graphics-preset very-low --video-memory-usage low
--max-texture-size 2048` reaches the rendered main menu and reports 222.48 MB
estimated texture VRAM and initialization at 69.038 seconds. The log still
reports 4096-sized atlases, so the requested 2048 cap is not treated as an
effective reduction. The first attempt to advance from New Game did not progress; the final
stat-corrected kernel with tablet input does not reproduce that problem. At
72.874 seconds it reaches the menu, starts the playable tutorial, accepts WASD
movement, writes a new 1,302,431-byte save and reloads it back to the moved
position. Three seconds of HDA capture contain nonzero audio (peak 1,358,
normalized RMS 0.00910). Normal WM_DELETE exits 0 with no live tasks and both
streams at EOF. Saving logs nonfatal chown EPERM warnings for temporary metadata
files; the save and reload complete nonetheless. The low-memory graphics
workaround and slow startup remain explicit limitations.

Warzone's installed executable is ELF64 and its own --help exits zero. Package
extraction succeeds, but profile linking initially fails because the review
disk's installer is the old 103,600-byte atomic-links binary, before the
already-committed standalone-output fix. The current installer repairs the
profile and exits zero. Its health check additionally treats a valid,
non-executable `bin/warzone2100.debug` as a corrupt loadable program and forces
an unnecessary re-download; only executable files are now checked as programs.
The regression retains damaged-executable detection. All 34 installer tests pass
with loopback networking enabled; five cancellation fixtures initially could
not create sockets under the execution sandbox.

The 115-test kernel suite initially times out in its 16-thread fd stress test
while the VM is running. A four-core isolated retry develops severe ticket-lock
convoying; allowing the idle host's 16 logical CPUs completes it successfully.
All other 114 tests pass, and compiled entry/start mutation audits pass. Do not
interpret the test's contention period as representative game performance.

The baseline review loses guest responsiveness and its user applications exit
during this contention period. Snapshot registers alone initially suggest a
scheduler-lock wait, but repeated lock counters advance and responsiveness
returns; no scheduler deadlock is established. Steam's collected exit is 1,
not a clean game/Steam acceptance pass. Original saves remain unchanged.
Checked VM stop, original launcher exit zero, serial check and all four offline
filesystem checks pass before cloning the candidate review.

## GPU handle capacity and Warzone acceptance

Warzone reaches its menu with the stat-corrected kernel, but loading the tutorial
aborts with OpenGL OUT_OF_MEMORY. Driver diagnostics show the 2,048-entry GEM
metadata table is full, while more than 7 GiB of guest RAM remains free. The
failure is a handle limit, not evidence of exhausted physical RAM or host VRAM.
The bounded per-context table is increased to 8,192 entries; dynamically growing
stable entries and explicit memory budgets remain future work.

The owned VM-only `gem-capacity` probe reproduces the baseline limit exactly:
2,048 live 4-KiB resources followed by ENOMEM, with close/reuse and cleanup passing.
On the candidate it creates 4,096 resources, maps and checks a resource beyond the
old limit, closes/reuses a slot and cleans up successfully. All 18 GPU host tests
and compiled entry/start mutation audits pass. Candidate kernel SHA256:
`1324e57216801b4d90fd1ee2ea0b8a11d5c3d0979d5c91a3e246295e11aa78e7`.

Warzone 4.7.0 then passes tutorial rendering and unit selection/order input. The
unit-order capture contains nonzero 44.1-kHz HDA samples (three seconds, peak
8,991, normalized RMS 0.0232); the idle capture was silent. This verifies audio
sample delivery, not subjective stutter-free playback. A fresh original campaign
renders its intro and map, saves to a new private slot, reloads that slot back to
the map, returns to the main menu and exits normally. The durable launch job
collects exit 0, no live tasks and EOF on both output streams. Tutorial mode has
no save menu, so save/load acceptance uses the campaign.

## Torchlight II: bounded gameplay, with launcher limitations

The mod launcher repeatedly waits for Steam, even after the client restores its
session. A three-second profile records 2,396 samples and 16,388 context switches,
without producer drops; it does not establish the cause of the API wait. A direct
engine launch without Steam's per-user connection fails initialization, as
expected. Connecting to the active client gets farther, but the engine exits
with a UTF-8 locale error even when LANG/LC_ALL request C.UTF-8.

The pinned glibc's minimal en_US.UTF-8 archive fixes that explicit startup error.
`nix-build tools/locale-brook-pkg --no-out-link` produces a 3,063,024-byte archive
at `lib/locale/locale-archive`; use that path as LOCALE_ARCHIVE with
LANG/LC_ALL=en_US.UTF-8. Its SHA256 is
`4c3b8c3ae5e03113701a5e760ca2135573679e212e048a578d04c1651cee43c0`.
This package is reproducible source, not a copied host locale archive. The
review transfers and verifies the archive separately; fresh images do not yet
select it automatically.

The controlled native engine launch then renders character creation and Echo
Pass. A new BrookTest character accepts click-to-move input; Save and Quit returns
to the title screen, and Resume/Single Player reloads the character at the moved
position. Three seconds of HDA capture contain audio (peak 7,908, normalized RMS
0.0567). This is bounded sample delivery and in-session resume, not a human audio
quality verdict or a cold-boot save-durability test. The game writes under its
game-relative `my games` directory despite the requested separate HOME, so HOME
alone is not a reliable save-isolation mechanism for this title.

The game window closes and the engine eventually collects exit 0, no live tasks
and output EOF. Steam IPC assertions repeat during the run. The separate stalled
mod launcher remains after normal Steam shutdown and requires SIGTERM; only then
does the Steam root complete with exit 0, zero live tasks and both streams at
EOF. Thus the direct engine gameplay result is not a normal Steam launcher pass,
and fully automatic launch/cleanup remains open. No authentication or game data
is included in the repository.

## Evidence and closure

The README includes account-free original-pixel crops of the three games. No
upscaling, game assets, saved games or account logs are committed. Private evidence is under `build/workloads/three-games-20261026` and
`build/reviews/three-games-stat-20261026/modal-control`; previous checked controls
are retained in that review's archived control directories.


After application cleanup, original Stardew save contents (33 files), Goose
save content/mtime and both tracked Graveyard Keeper save contents remain
unchanged; the known Stardew marker metadata exception remains separate from
non-marker files. The VM acknowledges checked filesystem/device flush and stops.
The owner exec is observed terminated with status 143, so an original launcher
exit-0 receipt is unavailable and is not claimed. Independent verification first
confirms QEMU has exited, then passes the stopped serial check and all four
read-only filesystem checks (data, Nix, tmp and boot each return 0). The private
images and evidence are preserved; the owned HTTP staging server is stopped.

This closes the bounded three-game pass. Remaining work is throughput,
Factorio's large texture backing failure at higher presets, and Torchlight's
normal Steam handoff/cleanup and cold-boot save durability.
