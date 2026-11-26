# Brook Steam

The personal `steam` app descriptor requires `--allow-unfree`. The profile
ships a Nix-pinned Valve x86-64 client and SteamRT3c seed, an Xwayland launcher,
and its pinned host loader/cache. The launcher runs a private PulseAudio
server and withdraws it on exit. Its current default is the existing null sink.

The packaged client uses the hashes and lengths recorded in
[`valve-client.json`](valve-client.json). The manifest digest records which
public Valve update-service response supplied those pins. The build checks
every downloaded archive, the actual x86-64 executable and the expected
SteamRT3c platform and launch tools. It uses no legacy or user-owned Steam
installation or files from a logged-in account.

On first launch the package publishes a writable copy under
`/data/home/.local/share/brook/steam-client`. Steam keeps the account and
games there. Later launches and profile updates retain the existing client,
games and login. An older hand-staged Steam installation stays where it was.
The store closure contains only the seed, so staging a profile never indexes
or copies login or game data. The standalone 32-bit client and 32-bit games
are outside this package's support.

Run `python3 scripts/test_brook_steam_seed.py` to verify first-launch
publication and preservation of synthetic user state.
