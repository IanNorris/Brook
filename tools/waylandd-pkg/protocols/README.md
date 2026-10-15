KDE server-decoration v1, vendored unchanged from the official plasma-wayland-protocols repository on 2026-10-15:

https://github.com/KDE/plasma-wayland-protocols/blob/master/src/protocols/server-decoration.xml

Copyright 2015 Martin Gräßlin; LGPL-2.1-or-later. The upstream license text is included here. XML SHA-256: `c5f857734bb190dc9adfb65fb05550a6245f12af1a2be5eefd05be199805a68c`.

GTK3 uses this protocol to discover the compositor default and announce each surface's decoration mode. Brook advertises SERVER, honors CLIENT and NONE, and leaves nonparticipants on the existing inferred-CSD path. Combining KDE and xdg-decoration on the same surface is undefined by the upstream protocol.
