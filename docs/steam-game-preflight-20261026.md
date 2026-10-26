# Next Steam game preflight — 26 October 2026

No additional game payload was downloaded during this preflight. Steam's local
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
