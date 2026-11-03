# Steam client bootstrap

`bootstrap.py` downloads Valve's 64-bit Steam client packages directly from
the official update service. It verifies each package against the downloaded
manifest, extracts it into a new private home and adapts ELF interpreters to
an installed Brook-compatible Nix loader. It does not download games, copy an
existing account, authenticate, or start the client.

Run it as the desktop user with a full Python 3 installation (including zlib,
ctypes and zipfile), curl, patchelf and the intended 64-bit dynamic loader:

```sh
python3 bootstrap.py --loader /nix/store/...-glibc/lib/ld-linux-x86-64.so.2
```

The default destination is `/data/home/steam-20261022`, matching the existing
Brook SteamRT launcher. An existing destination is never replaced. If a
download or extraction fails, the printed private staging directory remains for
inspection; remove it deliberately before retrying. The package hashes verify
the downloads against the manifest, whose authenticity relies on HTTPS.

Staging the client does not by itself make Steam usable. Xwayland, its Brook
window-manager integration, the SteamRT provider cache and a PulseAudio setup
are separate dependencies. Client updates, changing Valve package formats and
32-bit games remain unverified on the Dell.

Run the local parser, archive and publication checks with:

```sh
python3 -m unittest discover -s tools/steam-bootstrap -p 'test_*.py' -v
```
