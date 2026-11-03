# Dell Steam software GLX checkpoint

On the XPS 13 9343, Valve's updater completed outside the inner SteamRT
container. Its package moves cross separate mounts inside that container and
fail with `EXDEV`; bypassing the VFS mount boundary is not a suitable fix.
The updated client opted into SteamRT 64-bit after restoring the appropriate
client flag and patching the updated executable interpreters.

The normal software Xwayland session did not expose GLX. An isolated Xephyr
server, running inside that session with `+extension GLX`, `+iglx`, Mesa's
software driver path and the Mesa vendor library available, reported GLX 1.4
and returned a visual from `glXChooseVisual`. Its private mount namespace
provided `/bin/sh` and a disk-backed temporary directory for its keyboard map
and lock; these experiments did not change the system-wide library cache.
Steam's private host library directory also needed the Nix GnuTLS-compatible
Nettle and Hogweed versions to avoid mixing them with older runtime libraries.

One client launched against the nested display got past the earlier GLX and
TLS errors and started its web helper. Shortly afterward the Dell stopped
responding to input and its debug connection was lost. The saved log does not
establish whether the cause was process exhaustion, graphics, or another
failure. A usable login window was not confirmed. Do not repeat this launch
without a bounded way to capture resource use and stop it before another
unrecoverable hang. The subsequent forced power-off required an offline ext2
repair; the SSD and filesystem identities were verified before preen, and a
separate full read-only check passed afterward.

This nested setup is a diagnostic path, not a finished desktop integration.
It uses software rendering, a separate X desktop and incomplete audio/session
integration. The ordinary Xwayland/GLX path and Steam's long-term launcher
still need proper fixes.
