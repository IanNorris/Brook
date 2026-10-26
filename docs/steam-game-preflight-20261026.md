# Next Steam game preflight — 26 October 2026

The initial metadata pass downloaded no additional game payload. Steam's local
public appinfo launch/depot metadata, cached on 26 October, supplies the launch
entries below. Metadata is a candidate filter; actual executable headers and
the launch chain remain the final architecture gate. Linux availability alone,
a shell launcher, or Proton support does not establish 64-bit compatibility.

## Recommended mix

- **Graveyard Keeper (599140), native Linux first.** Explicit 64-bit launch
  entry `Graveyard Keeper.x86_64`, alongside a separate 32-bit entry. Linux depot
  599143 has a public manifest, approximately 447 MiB transferred / 1.68 GiB
  unpacked. Check ELF class and interpreter/dependencies before installation
  acceptance. The Windows entry does not declare its architecture.
  [Official store](https://store.steampowered.com/app/599140/Graveyard_Keeper/).
- **Untitled Goose Game (837470), Windows through Proton.** Explicit Windows
  64-bit entry `Untitled.exe`; no native Linux launch. Windows depot 837471 is
  approximately 265 MiB transferred / 830 MiB unpacked. The publisher also
  specifies 64-bit Windows and DirectX 10. Check PE machine type and any
  launcher/prerequisite helpers before a full run; Steam's integrated Proton
  launch remains a test target rather than assumed acceptance.
  [Publisher requirements](https://goose.game/support/),
  [official store](https://store.steampowered.com/app/837470/Untitled_Goose_Game/).

Sizes are cached public-depot values, not guaranteed total downloads: shared
runtime, language, update and optional-content requirements can add to them.

## Hold full downloads pending a narrower check

- **Half-Life (70).** Linux launcher is `hl.sh`; neither that entry nor
  `hl.exe` declares architecture. Inspect the engine executable, not the script.
  The Linux platform depot 8 is about 40.5 MiB transferred / 122 MiB unpacked,
  so a selected-file/header check should precede common game content. Do not
  substitute Half-Life 2 architecture information for the original game.
  [Official store](https://store.steampowered.com/app/70/HalfLife/).
- **Torchlight (41500).** Cached launch entries include both Linux x86 and
  x86_64 binaries, but Linux depot 41504 has no cached public manifest and the
  common platform list is Windows/macOS. Establish whether a usable Linux
  depot is actually available before treating the x86_64 entry as installable.
  Windows `Torchlight.exe` has no architecture declaration in this metadata.
  [Official store](https://store.steampowered.com/app/41500/Torchlight/).
- **Left 4 Dead (500), original game.** Cached launch entries and the store
  list Windows/macOS, with no native Linux entry. `left4dead.exe` has no
  architecture declaration. Check its PE header first; the Windows platform
  depot 503 is only about 2.6 MiB transferred / 6.3 MiB unpacked, before much
  larger shared/language content. Do not infer original-game support from
  Left 4 Dead 2.
  [Official store](https://store.steampowered.com/app/500/Left_4_Dead/).

If selected-file retrieval is needed, DepotDownloader supports file lists and
manifest-only retrieval; anonymous access is not guaranteed for licensed depots.
Keep any authentication and retrieved binaries private. A mandatory 32-bit
engine or helper puts that launch path outside the current 64-bit-only scope.
[Upstream retrieval options](https://github.com/SteamRE/DepotDownloader#usage).

## Bounded test results

Both selected games' installed launch chains were verified 64-bit. Graveyard
Keeper's native Unity player ignores ENOTSUP from a recursive PI mutex
initialization and then blocks locking that mutex. Untitled Goose Game passes
Wine's original preloader after the interpreter-less PIE placement fix, but its
X11 Vulkan presentation fails by default; an explicit software-presentation
control creates a white, silent window and does not complete normal close.
Neither game is marked compatible. See [measurements and diagnostic controls](loading-throughput-20261026.md)
for evidence, limitations and save-preservation checks. Full PI futex support
and the Proton presentation/stall remain separate follow-up work.

The [subsequent PI implementation](pi-futex-implementation.md) and
[presentation checkpoint](steam-api-and-present-wait-checkpoint.md) supersede
those initial blockers: native Graveyard gets past PI/Mono initialization,
and Goose now renders interactive menus with the isolated Mesa candidate.
The [stream-write and gameplay checkpoint](unix-stream-writev-checkpoint.md)
subsequently verifies a new Goose tutorial, input, audio responses and normal
menu-driven exit. Graveyard Vulkan renders its complete menu and delivers
audio, but input and resize behavior still need work. Neither result establishes
full-game compatibility.

## Actual platform-depot checks

The signed-in guest Steam console retrieved only the small platform depots,
without their common game-content depots. Executable headers were read inside
the private guest; no downloaded binaries or account material are committed.

- **Half-Life 70, Linux depot 8**, manifest `1360709870205537514`:
  `hl.sh` sets `GAMEEXE=hl_linux`. That executable is ELF32, machine 3 (i386),
  SHA256 `090e4e3e97ab6ba8264e0666bd2e2909fa1218a46fb5655872182333fbddc671`.
  Several accompanying libraries, including `hw.so` and `libsteam_api.so`,
  are also ELF32. The presence of a separate `engine_amd64.so` does not make
  the configured client launch chain 64-bit. This path is outside current scope.
- **Original Left 4 Dead 500, Windows depot 503**, manifest
  `5156732173895269546`: its `left4dead/bin/client.dll` is PE i386
  (`Machine=0x14c`, PE32 optional-header magic `0x10b`), SHA256
  `7612545f68a6b670fb4d4a99a79c69f2d3939ebb80c15c1fd5c2b8c864f4daeb`.
  The accompanying Bink and CRT DLLs are also PE32. The launcher EXE is not in
  this platform depot and was not inspected; the actual client DLL already
  excludes this downloaded client path from a 64-bit-only test. No substitution
  of Left 4 Dead 2 was made.
- **Torchlight 41500, Linux depot 41504**: the current Steam console returns
  `Invalid default manifest (Missing configuration)`. No files were retrieved.
  Its cached x86_64 launch entry remains metadata, not an available installation.

The first two depots completed with 48 files / 127,612,567 unpacked bytes and
3 files / 6,634,648 unpacked bytes respectively. No full downloads or launches
of these three excluded paths were attempted. These findings apply to the
specific retrieved manifests and configured client paths, not every historical
version, branch or community port.
