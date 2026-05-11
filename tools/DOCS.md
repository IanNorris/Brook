# Userspace Tools Documentation

## Overview

Brook's userspace tools provide package management (Nix binary cache
integration), Wayland applications, and test programs.

## Package Management

| Tool | Lines | Purpose |
|------|-------|---------|
| nix-install | 1228 | Package installer: index search, dependency resolution, PATH setup |
| nix-fetch | 801 | Download/extract packages from cache.nixos.org via curl + nar-unpack |
| nar-unpack | 265 | NAR archive extractor (Nix's binary archive format) |
| nix-search | 140 | Search local package index |
| nix-index | — | Build local package index from store |

### Package Install Flow
```
nix-install <pkg>
  → nix-search (find store hash in index)
  → nix-fetch --deps <hash> (download + extract all deps)
    → curl https://cache.nixos.org/<hash>.narinfo
    → curl <nar-url> | xz -d | nar-unpack /nix/store/<hash>
  → symlink /nix/profile/bin/<binary> → store path
  → update PATH and PS1
```

## Wayland Applications

| Tool | Lines | Purpose |
|------|-------|---------|
| waylandd | ~2200 | Wayland compositor relay (protocol → kernel WM, subsurfaces, popups) |
| brook-files | ~1330 | Two-pane file browser with toolbar, breadcrumbs, column sorting |
| brook-edit | ~900 | Text editor with save/load, Tab, Ctrl+D duplicate |
| brook-player | 764 | Audio/video player |
| brook-console | 600 | Kernel log viewer |
| wayland-calc | 476 | Calculator |

## Test Programs

| Tool | Lines | Purpose |
|------|-------|---------|
| wayland-xdg-smoke | 367 | XDG shell protocol validation |
| wayland-shm-smoke | 179 | wl_shm protocol validation |
| brook-fbtest | 332 | Framebuffer stress test |
| sinetest | 111 | Audio sine wave test |
| wavplay | 142 | WAV file playback |
| mp3play | 176 | MP3 playback (minimp3) |

## Audit Findings (2026-05-10)

### Known Issues
- **BRO-129** (fixed): nar-unpack symlink targets allowed path traversal
- **BRO-130**: nix-install strcpy/strcat buffer overflows in PATH/PS1 construction
- nix-fetch uses predictable temp file names in /tmp (TOCTOU)
- mp3play/wavplay don't check ioctl() return values
- waylandd protocol handling is complex (~2000 lines) and needs targeted review

### Overall Assessment
**Reasonable quality for hobby OS tools.** The package management pipeline
handles untrusted remote data (NAR archives) — the symlink traversal fix
was the most important security hardening. The Wayland applications are
well-structured with proper callback patterns.
