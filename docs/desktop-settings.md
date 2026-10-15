# Desktop appearance and installed applications

The desktop has three shared palettes: Brook, Dawn and Midnight. The GTK
Settings app previews the same palette definitions used by native window
chrome, the launcher and taskbar. Applying a theme saves `[Desktop] theme=N`
to `/data/home/.config/brook/settings.ini`; startup restores that selection.
The current settings path represents the persistent desktop account, rather
than a complete multi-user settings service. Third-party toolkit themes are
independent of the Brook chrome palette.

Settings uses GTK3 for its contents and requests server-side decorations so
Brook draws its title bar. The Wayland server advertises the standard KDE
server-decoration protocol used by GTK3, with server decorations as the
default. Applications that explicitly request their own header bars keep
them; the protocol support is shared by GTK applications rather than tied
to the Settings application ID.

Build Settings with `nix-build tools/brook-settings-pkg --no-out-link` inside
`nix-shell`. Fresh experiment profiles include it. Existing images can install
an exported `brook-settings` package through the guest installer.

Application icons are 24×24 straight-alpha ARGB assets generated from package
PNG/SVG/XPM icons. Original built-in icons live in `data/icons`. Fresh experiment
images run the icon collector; launcher manifests include the desktop ID so
Wayland's `app_id` can identify a window even when its document title changes.
Native title bars show app icons, and running-window taskbar buttons use icons
with focus/minimized indicators and a hover label. Missing icons use a generic
application glyph.

The installer exports and removes desktop entries through the active profile,
including migration of packages installed with older tools. Cached store files
alone do not make an application installed. Custom boot shortcuts declare
`# Requires:` dependencies; unavailable shortcuts are hidden. The generated
manifest is published by durable temporary-file replacement and followed by a
refresh notification. One maintenance worker builds immutable launcher snapshots;
the compositor publishes them after input against the preceding frame, and
the worker reclaims retired icons after their readers finish.

The current launcher accepts 128 entries, a 64 KiB manifest and 1 KiB records;
commands remain bounded by the existing 128-byte launcher command field.
Malformed or oversized candidates retain the previous menu with a diagnostic.
Runtime image decoding for newly installed icons that were not preconverted,
terminal-only desktop-entry launching, smaller-display acceptance, and a
graphical Nix app store remain followups.

## Verified guest acceptance, 15 October 2026

Kernel SHA256:
`6aa506f25cf667c524f1bfe0ee2063edd67f660b913ade322320acdda2dc9798`.
The test used one isolated 8-CPU, 4 GiB VM at 1920×1080 with audio disabled.

- Settings applied Dawn, saved it, and restored it after reboot.
  `wmthemecheck 1` also verified invalid selections leave the theme unchanged.
- Native chrome and launcher icons rendered; Mousepad retained its real app
  icon with an `Untitled` document title.
- Repair, removal and reinstallation of Settings updated an open launcher
  through 16 → 15 → 16 entries. Removal left the store executable cached but
  removed the profile executable and launcher entry.
- A 65,537-byte manifest was rejected. The visible menu's screenshot hash
  stayed unchanged; restoring the valid manifest recovered publication.
- Shutdown completed checked filesystem/device flushes; offline FAT and both
  ext2 checks returned zero. The host test suite passed all 13 tests.

Local evidence is retained under
`build/workloads/icons-theme-20261015/final-acceptance.json` and adjacent logs.

The subsequent GTK3 decoration check used kernel
`8729d7ee097d225dbc093f775bdacfe21104ea5b20d70b87adc731db1de5d7fe`
and bridge session `cc7586f7700f4852ab3cee0a64e188c6`. Settings received
server-side decorations, passed maximize/restore and dragging, applied Midnight
to its actual title bar and the taskbar, and closed/reopened successfully.
Mousepad explicitly requested client-side decorations and retained a single
header bar. Full 1920×1080 captures are retained in that session's artifacts.
