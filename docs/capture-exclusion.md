# Capture exclusion

A Wayland application can bind `brook_capture_manager_v1` and send
`exclude` with its xdg-toplevel's `wl_surface` before submitting private
pixels. The protocol XML is in `tools/waylandd-pkg/protocols`. The exclusion
lasts until that surface is destroyed. Its xdg-popup descendants inherit it.

Add rules to `/etc/capture-exclude.conf` with an app ID glob, a tab, and a title
glob per line. Both patterns must match. Patterns use ASCII case-insensitive
matching with `*` and `?`. A line starting with `#` is a comment. Restart the
desktop to load edits. The first match latches the exclusion for the window.

Built-in rules cover KeePassXC, GNOME Secrets, and browser title hints.
Firefox's English private title can be detected when the suffix is present
in the WM title. Chromium's Incognito annotation can be confined to its
accessibility title. Use an app ID with a wildcard title to exclude all of a
browser's windows if private titles cannot be distinguished.

Completed software captures fill the excluded window rectangles with opaque
black. Single-window buffer and crop requests fail for an excluded window.
When a flag is set, older capture leases are revoked. Capture requires a
newly composed frame after exclusion has been used. A GPU readback or
renderer-owned scanout without matching scene geometry is withheld.
Use kernel-verified capture endpoints for screenshots. Host hypervisor
framebuffer inspection is outside the Brook compositor's control.
