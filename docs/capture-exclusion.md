# Capture exclusion

A Wayland application can bind `brook_capture_manager_v1` and send
`exclude` with its xdg-toplevel's `wl_surface` before submitting private
pixels. The protocol XML is in `tools/waylandd-pkg/protocols`. The exclusion
lasts until that surface is destroyed. Its xdg-popup descendants inherit it.

Add rules to `/etc/capture-exclude.conf` with an app ID glob, a tab, and a title
glob per line. Both patterns must match. Patterns use ASCII case-insensitive
matching with `*` and `?`. A line starting with `#` is a comment. Reboot to
load edits. The first match latches the exclusion for the window and its
popups. Subsurface pixels inherit the root surface's protection. Titles are
matched before shortening the display label. An oversized policy title
latches exclusion.

Built-in rules cover KeePassXC, GNOME Secrets, and browser title hints.
Firefox's English private title can be detected when the suffix is present
in the WM title. Chromium's Incognito annotation can be confined to its
accessibility title. Use an app ID with a wildcard title to exclude all of a
browser's windows if private titles cannot be distinguished.

Completed captures fill the excluded window rectangles with opaque black.
Single-window buffer and crop requests fail for an excluded window. Setting
a flag revokes older capture leases.

The desktop renderer publishes the rectangles from its retained scene with
each GOP frame, Broadwell flip buffer, and zero-copy image. A fullscreen
direct-scanout buffer inherits the exclusion of its admitted window. Capture
uses the metadata of the actual live buffer even if the window has moved in
the meantime. A change to the mask requires a full renderer frame before
partial updates can resume. Capture withholds any image whose frame metadata
is unavailable or predates the latest exclusion.
Use kernel-verified capture endpoints for screenshots. Host hypervisor
framebuffer inspection is outside the Brook compositor's control.
