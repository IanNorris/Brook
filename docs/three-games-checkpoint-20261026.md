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
Input/gameplay/audio/save acceptance remains pending; the default 1920×1080
window extends beyond the desktop and needs a smaller game configuration.

Factorio's first startup fails in `std::filesystem::status` with EBADF for an
empty pathname. A scoped file trace ends with `newfstatat(AT_FDCWD, ..., ...) =
-9`. An owned static probe reproduces incorrect empty-path and flag behavior.
The kernel correction returns ENOENT without AT_EMPTY_PATH, handles cwd and
descriptor empty-path metadata separately, and follows Linux 6.12's NULL-path
fast-path distinctions. Review also identified that cwd metadata must use a
retained path without requiring directory search permission. The expanded
control passes on Linux; final native verification remains pending.

The first corrected Factorio launch initializes hardware VirGL OpenGL 4.6 and
private PulseAudio at 44.1 kHz, then reports GL_OUT_OF_MEMORY during atlas
creation. Kernel logs show resource backing attachment failures for large
resources; the rolling allocator failure counter is zero. This is not evidence
of exhausted host VRAM or failed physical allocation. A bounded retry with
`--force-graphics-preset very-low --video-memory-usage low
--max-texture-size 2048` reaches the rendered main menu and reports 222.48 MB
estimated texture VRAM and initialization at 69.038 seconds. The log still
reports 4096-sized atlases, so the requested 2048 cap is not treated as an
effective reduction. Gameplay/save/audio acceptance remains in progress.

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

## Acceptance in progress

No gameplay result is claimed by this checkpoint yet. Private evidence
is under `build/workloads/three-games-20261026` and
`build/reviews/three-games-20261026/modal-control`.
