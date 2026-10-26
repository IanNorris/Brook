# Owned Steam API controls

Build both with `nix-build tools/steam-api-probe-pkg -A windows -A linux --no-out-link`.
These programs dynamically load caller-supplied libraries; no Steam SDK, game
binary, account data or saves are included or linked into the outputs.

The Win64 program accepts `DLL_PATH SteamFriends017`, or reads those two lines
from `brook-steam-api-probe.cfg` beside its executable. Its log is written there
as `brook-steam-api-probe.log`. It loads the real API DLL, calls SteamAPI_Init,
gets the versioned Friends interface, calls GetPersonaName twice, pumps
callbacks briefly and shuts the API down. Logs contain phase timings, Windows
PID/TID and null/nonempty booleans; persona strings are never printed.

To test a game's actual Steam launch context in a **disposable private review**,
the owned executable can temporarily occupy the configured game launch path.
First preserve the original executable and record its hash, mode and timestamp.
Verify both the backup and installed probe before launch. After all diagnostic
tasks have exited, restore and verify the exact original before launching the
real game again. This is an API control, not game acceptance. Keep the private
DLL/configuration, originals and raw Proton logs outside git.

The Linux x86-64 control accepts `STEAMCLIENT_SO SteamFriends017`. With an active
native client, run it in the same runtime/container context being diagnosed.
It opens that exact library, obtains SteamClient020, creates a pipe, connects
the global user, obtains Friends, calls GetPersonaName twice and releases the
user/pipe. It invokes the verified SysV ABI prefix directly. This is a lower-
level native comparison, not the identical Windows SteamAPI_Init frontend.
Record namespace/environment differences rather than assume context equality.

The native method slots and signatures are documented in Proton's public
[SteamClient020 header](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_147/isteamclient.h)
and [Friends017 header](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_147/isteamfriends.h).
The interface prefix has no virtual destructor shifting its slots.

Both controls have a 30-second watchdog. Keep an external observer/deadline:
DLL loading, process teardown or other Wine services can outlive the main
sequence. Recover the original operation on observation timeout. API phase
completion, actual process exit and Steam's tracked-root exit are separate
results. A PASS marker checks return shapes only; it does not authenticate an
identity, prove callback content, or supersede an IPC assertion in private logs.
Never interpret an internal Steam IPC code as errno or another Steam enum
without evidence. Do not use these controls against the user's host desktop.
